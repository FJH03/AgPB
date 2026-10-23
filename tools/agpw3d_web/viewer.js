import * as THREE from './vendor/three.module.js';
import { OrbitControls } from './vendor/OrbitControls.js';
import { WaypointEditor } from './waypoint_editor.js';

// Keep Source XYZ coordinates unchanged. Z is always up.
const $ = (id) => document.getElementById(id);
const WP_FLAGS = [
  [1 << 1, 'LIFT'], [1 << 2, 'CROUCH'], [1 << 3, 'CROSSING'], [1 << 4, 'GOAL'],
  [1 << 5, 'LADDER'], [1 << 6, 'RESCUE'], [1 << 7, 'CAMP'], [1 << 9, 'DJUMP'],
  [1 << 11, 'AVOID'], [1 << 12, 'USEBUTTON'], [1 << 17, 'FALLRISK'],
  [1 << 26, 'FALLCHECK'], [1 << 27, 'JUMP'], [1 << 28, 'SNIPER'],
  [1 << 29, 'T'], [1 << 30, 'CT'],
];
const PATH_FLAGS = [[1, 'JUMP'], [2, 'DOUBLE'], [4, 'VISIBLE']];
const FLAG_BITS = Object.fromEntries(WP_FLAGS.map(([bit, name]) => [name, bit]));
// Match the in-game waypoint colors in wpdraw.cpp exactly.
const NODE_COLORS = [
  ['CAMP', '#00FFFF'], ['GOAL', '#8000FF'], ['LADDER', '#804000'],
  ['RESCUE', '#FFFFFF'], ['AVOID', '#FF0000'], ['FALLCHECK', '#808080'],
  ['USEBUTTON', '#0000FF'], ['FALLRISK', '#808080'], ['JUMP', '#FFFF00'],
  ['CROUCH', '#AA00FF'], ['LIFT', '#008080'],
];
const NODE_OVERLAYS = [
  ['SNIPER', '#825700'], ['T', '#FF0000'], ['CT', '#0000FF'], ['FALLRISK', '#FA4B96'],
];
const EDGE_COLORS = ['#8a8a8a', '#e05252', '#4a7fd4', '#3fa34d'];
const clipPlane = new THREE.Plane(new THREE.Vector3(0, 0, -1), 1e10);
const up = new THREE.Vector3(0, 0, 1);
const yAxis = new THREE.Vector3(0, 1, 0);
const projected = new THREE.Vector3();
const raycaster = new THREE.Raycaster();
const pointer = new THREE.Vector2();
const state = {
  data: null, root: null, models: null, points: [], incoming: [], nodes: null,
  selected: -1, selection: null, selectionRadius: null, labels: [], waypointMaterials: [], mapMaterials: [],
  markerOverlays: [], markerScales: new Float32Array(0), arrowSets: [],
  mapBounds: null, waypointBounds: null, sliceMin: 0, sliceMax: 1,
  topView: false, orthoSpan: 600, savedPerspective: null,
  ready: false, loading: false, renderNeeded: true, labelNeeded: true,
  stats: {}, error: null, geometryCache: new Map(), textureCache: new Map(), imported: false,
  materialCache: new Map(), materialDefinitions: new Map(), loadedTextures: new Map(),
  editor: null, saveHandle: null, saving: false,
};
const fly = {
  enabled: false, yaw: 0, pitch: 0, speed: 240, orbitDistance: 300,
  keys: new Set(), pointerId: null, lastX: 0, lastY: 0,
  lockStatus: 'off', lockError: '', lockErrorDetail: '', session: 0, inFlight: null, engaged: false,
};
const FLY_KEYS = new Set(['KeyW', 'KeyA', 'KeyS', 'KeyD', 'KeyQ', 'KeyE', 'ShiftLeft', 'ShiftRight']);
let renderer, scene, camera, controls;
let previousFrameTime = null;
const perspective = new THREE.PerspectiveCamera(50, 1, 1, 200000);
const orthographic = new THREE.OrthographicCamera(-300, 300, 300, -300, .1, 200000);
perspective.up.copy(up);
orthographic.up.copy(up);
camera = perspective;

// Source mathlib AngleMatrix; pitch, yaw, roll in degrees.
// THREE.Matrix4.set takes row-major arguments, avoiding Euler-order guesses.
function sourceAngleMatrix(origin = [0, 0, 0], angles = [0, 0, 0], scale = 1) {
  const p = THREE.MathUtils.degToRad(angles[0] || 0);
  const y = THREE.MathUtils.degToRad(angles[1] || 0);
  const r = THREE.MathUtils.degToRad(angles[2] || 0);
  const sp = Math.sin(p), cp = Math.cos(p), sy = Math.sin(y), cy = Math.cos(y);
  const sr = Math.sin(r), cr = Math.cos(r), s = Number.isFinite(scale) ? scale : 1;
  return new THREE.Matrix4().set(
    cp * cy * s, (sr * sp * cy - cr * sy) * s, (cr * sp * cy + sr * sy) * s, origin[0],
    cp * sy * s, (sr * sp * sy + cr * cy) * s, (cr * sp * sy - sr * cy) * s, origin[1],
    -sp * s, sr * cp * s, cr * cp * s, origin[2],
    0, 0, 0, 1,
  );
}
function flagsText(value, table) {
  const names = table.filter(([bit]) => (value & bit) !== 0).map(([, name]) => name);
  const known = table.reduce((bits, [bit]) => bits | bit, 0);
  const unknown = (value & ~known) >>> 0;
  if (unknown) names.push(`0x${unknown.toString(16).toUpperCase()}`);
  return names.length ? names : ['无'];
}
function edgeClass(flags) { return (flags & 1) ? 1 : (flags & 2) ? 2 : (flags & 4) ? 3 : 0; }
function nodeColor(flags) { return NODE_COLORS.find(([name]) => flags & FLAG_BITS[name])?.[1] || '#00FF00'; }
function setStatus(message) { $('status').textContent = message; }
function requestRender() { state.renderNeeded = true; state.labelNeeded = true; }
function boxFromBounds(bounds) {
  if (!bounds || !Array.isArray(bounds.min) || !Array.isArray(bounds.max)) return null;
  if (![...bounds.min, ...bounds.max].every(Number.isFinite)) return null;
  const box = new THREE.Box3(new THREE.Vector3(...bounds.min), new THREE.Vector3(...bounds.max));
  return box.isEmpty() ? null : box;
}
function element(tag, text, className) {
  const item = document.createElement(tag);
  if (text !== undefined) item.textContent = text;
  if (className) item.className = className;
  return item;
}
function descriptionList(parent, rows) {
  const list = element('dl');
  for (const [name, value] of rows) list.append(element('dt', name), element('dd', value));
  parent.append(list);
  return list;
}
function addFlags(parent, value, table) {
  for (const name of flagsText(value, table)) parent.append(element('span', name, 'flag'));
}
function mapMaterial(color, materialId = null) {
  const key = `${color}:${materialId}`;
  if (state.materialCache.has(key)) return state.materialCache.get(key);
  const definition = state.materialDefinitions.get(materialId);
  const texture = definition ? state.loadedTextures.get(definition.texture_id) || null : null;
  const textured = $('show-textures').checked && !!texture;
  // Only basetexture is used; no VMT alpha, color, transform or shader parameters.
  const material = new THREE.MeshLambertMaterial({
    color: textured ? '#ffffff' : color, map: textured ? texture : null,
    side: THREE.DoubleSide, clippingPlanes: [clipPlane], transparent: false, opacity: 1, alphaTest: 0,
  });
  material.name = definition?.name || 'gray-fallback';
  material.userData.baseTexture = texture;
  material.userData.grayColor = color;
  state.mapMaterials.push(material);
  state.materialCache.set(key, material);
  return material;
}
function surfaceMaterials(geometry, color, warnings) {
  const ids = geometry.userData.materialIds;
  if (!ids?.length) return mapMaterial(color);
  return ids.map((id) => {
    if (id !== null && !state.materialDefinitions.has(id)) warnings.push(`材质 #${id} 缺少定义，已使用灰模。`);
    return mapMaterial(color, id);
  });
}
function applyTextures() {
  for (const material of state.mapMaterials) {
    const textured = $('show-textures').checked && !!material.userData.baseTexture;
    material.map = textured ? material.userData.baseTexture : null;
    material.color.set(textured ? '#ffffff' : material.userData.grayColor);
    material.needsUpdate = true;
  }
  requestRender();
}
function waypointMaterial(kind, options = {}) {
  const material = new kind({ transparent: true, ...options, clippingPlanes: [clipPlane], depthTest: !$('xray').checked });
  state.waypointMaterials.push(material);
  return material;
}
function disposeTree(root) {
  if (!root) return;
  const geometries = new Set(), materials = new Set();
  root.traverse((object) => {
    if (object.geometry && !object.geometry.userData.cached) geometries.add(object.geometry);
    if (object.material) for (const material of [].concat(object.material)) materials.add(material);
    if (object.isInstancedMesh) object.dispose();
  });
  for (const geometry of geometries) geometry.dispose();
  for (const material of materials) material.dispose();
  root.removeFromParent();
}
function viewportFocused() {
  return !!renderer && document.activeElement === renderer.domElement;
}
function resetFlyInput() {
  fly.keys.clear();
  if (renderer && fly.pointerId !== null && renderer.domElement.hasPointerCapture(fly.pointerId)) {
    renderer.domElement.releasePointerCapture(fly.pointerId);
  }
  fly.pointerId = null;
  renderer?.domElement.classList.remove('fly-looking');
  previousFrameTime = null;
}
// Pointer Lock follows MDN's event-based contract, including legacy void returns.
// https://developer.mozilla.org/en-US/docs/Web/API/Pointer_Lock_API
// https://developer.mozilla.org/en-US/docs/Web/API/Element/requestPointerLock
function canvasPointerLocked() {
  return !!renderer && document.pointerLockElement === renderer.domElement;
}
function releaseFlyPointerLock() {
  fly.session++;
  if (fly.inFlight) fly.inFlight.cancelled = true;
  fly.lockStatus = 'off';
  fly.lockError = '';
  fly.lockErrorDetail = '';
  fly.engaged = false;
  if (canvasPointerLocked()) {
    try { document.exitPointerLock(); }
    catch (error) {
      fly.lockError = '无法释放鼠标，请按 Esc 退出接管。';
      fly.lockErrorDetail = error.message || String(error);
      console.error('Pointer lock release failed:', error);
    }
  }
}
function pointerLockFailureReason(error) {
  switch (error?.name) {
    case 'SecurityError': return '浏览器安全限制阻止接管鼠标。';
    case 'NotAllowedError': return '浏览器未允许接管鼠标。';
    case 'NotSupportedError': return '当前浏览器不支持鼠标接管。';
    case 'InvalidStateError': return '当前页面暂时无法接管鼠标，请点击画面重试。';
    case 'AbortError': return '鼠标接管已取消，可点击画面重试。';
    default: return '当前浏览器无法接管鼠标。';
  }
}
function failFlyPointerLock(request, error) {
  if (fly.inFlight === request) fly.inFlight = null;
  if (!fly.enabled || request.cancelled || request.session !== fly.session
    || (fly.inFlight && fly.inFlight !== request) || canvasPointerLocked()) return;
  resetFlyInput();
  fly.lockStatus = 'fallback';
  fly.lockError = pointerLockFailureReason(error);
  fly.lockErrorDetail = error?.message || String(error || '');
  updateFlyUI();
  setStatus(`未锁定，可左键拖动或点击重试。${fly.lockError}`);
  console.warn('Pointer lock request failed:', error);
}
function handleFlyPointerLockChange() {
  if (canvasPointerLocked()) {
    const request = fly.inFlight;
    const validRequest = request && !request.cancelled && request.session === fly.session;
    if (fly.enabled && state.ready && !state.loading && !document.hidden && viewportFocused()
      && (fly.lockStatus === 'locked' || validRequest)) {
      if (validRequest) fly.inFlight = null;
      resetFlyInput();
      fly.engaged = true;
      fly.lockStatus = 'locked';
      fly.lockError = '';
      fly.lockErrorDetail = '';
      updateFlyUI();
      setStatus('鼠标已接管：直接移动鼠标转向 · WASD / Q E 移动 · Shift 加速 · Esc 释放鼠标 · V 退出');
    } else {
      // A cancelled/old request must never recapture the user's mouse later.
      if (request) fly.inFlight = null;
      resetFlyInput();
      let releaseError = '';
      try { document.exitPointerLock(); }
      catch (error) {
        releaseError = '无法释放鼠标，请按 Esc 退出接管。';
        fly.lockErrorDetail = error.message || String(error);
        console.error('Stale pointer lock release failed:', error);
      }
      if (fly.enabled) {
        fly.lockStatus = 'fallback';
        fly.lockError = releaseError || '已取消过期的鼠标接管；可左键拖动或点击重试。';
        updateFlyUI();
        setStatus(fly.lockError);
      } else if (releaseError) {
        fly.lockError = releaseError;
        updateFlyUI();
        setStatus(releaseError);
      }
    }
    return;
  }
  if (fly.lockStatus === 'locked') {
    // Unlock can mean Esc, lost focus or browser UI; it is not a mode change.
    suspendFly();
  }
}
function requestFlyPointerLock() {
  if (!fly.enabled || state.loading || state.saving || !state.ready || document.hidden
    || !document.hasFocus() || !viewportFocused() || canvasPointerLocked()) return;
  fly.engaged = true;
  if (typeof renderer.domElement.requestPointerLock !== 'function') {
    fly.lockStatus = 'fallback';
    fly.lockError = '当前浏览器不支持鼠标接管。';
    updateFlyUI();
    setStatus('未锁定，可左键拖动。当前浏览器不支持鼠标接管。');
    return;
  }
  // Keep one outstanding browser request, even across mode exit/re-entry.
  // This prevents an old asynchronous success being mistaken for a new request.
  if (fly.inFlight) {
    fly.lockStatus = 'fallback';
    fly.lockError = '上次鼠标接管请求尚未结束，请稍后点击重试。';
    updateFlyUI();
    setStatus(`未锁定，可左键拖动。${fly.lockError}`);
    return;
  }
  const request = { session: fly.session, cancelled: false };
  fly.inFlight = request;
  fly.lockStatus = 'requesting';
  fly.lockError = '';
  fly.lockErrorDetail = '';
  resetFlyInput();
  updateFlyUI();
  setStatus('正在请求鼠标接管；尚未锁定。');
  try {
    // Called synchronously from the user's canvas click; no await before this.
    const result = renderer.domElement.requestPointerLock();
    if (result && typeof result.then === 'function') {
      result.then(() => {
        if (canvasPointerLocked()) handleFlyPointerLockChange();
        else if (fly.inFlight === request) failFlyPointerLock(request, new Error('浏览器未确认鼠标接管。'));
      }, (error) => failFlyPointerLock(request, error));
    }
    // A void return remains pending until pointerlockchange / pointerlockerror.
  } catch (error) { failFlyPointerLock(request, error); }
}
function rotateFlyView(deltaX, deltaY) {
  fly.yaw -= deltaX * .003;
  fly.pitch = THREE.MathUtils.clamp(fly.pitch - deltaY * .003,
    -THREE.MathUtils.degToRad(89), THREE.MathUtils.degToRad(89));
  applyFlyLook();
}
function flyMovementActive() {
  return !document.hidden && document.hasFocus() && viewportFocused()
    && ((fly.lockStatus === 'locked' && canvasPointerLocked())
    || (fly.lockStatus === 'fallback' && fly.engaged));
}
function suspendFly() {
  resetFlyInput();
  if (fly.enabled || canvasPointerLocked() || fly.inFlight) releaseFlyPointerLock();
  if (!fly.enabled) return;
  // Panel interaction pauses input without switching back to OrbitControls.
  fly.lockStatus = typeof renderer.domElement.requestPointerLock === 'function' ? 'waiting' : 'fallback';
  fly.lockError = fly.lockStatus === 'fallback' ? '当前浏览器不支持鼠标接管。' : '';
  updateFlyUI();
  setStatus('自由飞行保持开启：点击画面继续；Esc 释放鼠标，V 退出。');
  requestRender();
}
function flyDirection() {
  const cp = Math.cos(fly.pitch);
  return new THREE.Vector3(cp * Math.cos(fly.yaw), cp * Math.sin(fly.yaw), Math.sin(fly.pitch));
}
function applyFlyLook() {
  camera.up.copy(up);
  camera.lookAt(camera.position.clone().add(flyDirection()));
  requestRender();
}
function updateFlyUI() {
  const available = !!state.data?.map && state.ready && !state.loading;
  const locked = fly.enabled && canvasPointerLocked() && fly.lockStatus === 'locked';
  $('fly-mode').disabled = !available;
  $('fly-speed').disabled = !available;
  $('fly-mode').textContent = fly.enabled ? '退出自由飞行  V' : '自由飞行（可穿墙）  V';
  $('fly-mode').setAttribute('aria-pressed', String(fly.enabled));
  $('fly-speed-value').textContent = `${fly.speed} 单位/秒`;
  const messages = {
    waiting: '等待接管：点击画面锁定鼠标；也可左键拖动。',
    requesting: '正在请求接管，尚未锁定；请等待浏览器确认。',
    locked: '鼠标已接管：直接移动鼠标转向，Esc 释放鼠标，V 退出。',
    fallback: '未锁定，可左键拖动或点击重试。',
  };
  $('fly-lock-status').hidden = !fly.enabled && !fly.lockError;
  $('fly-lock-status').textContent = `${messages[fly.lockStatus] || ''}${fly.lockError ? ` ${fly.lockError}` : ''}`;
  $('fly-lock-status').classList.toggle('fly-lock-warning', !!fly.lockError);
  $('fly-crosshair').hidden = !locked;
  $('camera-readout').hidden = !state.data?.map;
  if (state.data?.map) $('mode-badge').textContent = fly.enabled
    ? `自由飞行 · ${locked ? '已接管鼠标' : '未锁定'}`
    : state.topView ? '正交俯视 · Z 向上' : '透视环绕 · Z 向上';
  renderer?.domElement.classList.toggle('fly-active', fly.enabled);
  renderer?.domElement.classList.toggle('fly-locked', locked);
  renderer?.domElement.setAttribute('aria-label', fly.enabled
    ? locked ? '地图三维预览，鼠标已接管：移动鼠标转向，WASD 与 Q E 移动，Esc 释放鼠标，V 退出'
      : '地图三维预览，自由飞行待接管：点击画面锁定鼠标，或左键拖动转向，V 退出'
    : state.topView ? '地图三维预览，正交俯视：左键平移，滚轮缩放'
    : '地图三维预览，环绕视角：左键旋转，Shift 加左键平移，滚轮缩放');
}
function setFlyMode(enabled, focus = true) {
  if (enabled === fly.enabled) {
    if (!enabled && canvasPointerLocked()) {
      resetFlyInput();
      releaseFlyPointerLock();
      updateFlyUI();
    }
    return;
  }
  if (enabled && (!state.ready || !state.data?.map || state.loading || state.saving)) return;
  resetFlyInput();
  if (enabled) {
    if (state.topView) setTopView(false);
    const direction = camera.getWorldDirection(new THREE.Vector3());
    fly.yaw = Math.atan2(direction.y, direction.x);
    fly.pitch = THREE.MathUtils.clamp(Math.asin(THREE.MathUtils.clamp(direction.z, -1, 1)),
      -THREE.MathUtils.degToRad(89), THREE.MathUtils.degToRad(89));
    fly.orbitDistance = THREE.MathUtils.clamp(camera.position.distanceTo(controls.target), 64, 2000);
    fly.enabled = true;
    fly.session++;
    fly.engaged = false;
    fly.lockStatus = typeof renderer.domElement.requestPointerLock === 'function' ? 'waiting' : 'fallback';
    fly.lockError = fly.lockStatus === 'fallback' ? '当前浏览器不支持鼠标接管。' : '';
    controls.enabled = false;
    camera.near = .5;
    camera.updateProjectionMatrix();
    applyFlyLook();
    if (focus) renderer.domElement.focus({ preventScroll: true });
    setStatus('自由飞行已准备：点击画面接管鼠标；未锁定时可左键拖动。Esc 释放鼠标，V 退出。');
  } else {
    // Resume orbiting around the present line of sight, preserving this position.
    const direction = camera.getWorldDirection(new THREE.Vector3());
    fly.enabled = false;
    releaseFlyPointerLock();
    controls.target.copy(camera.position).addScaledVector(direction, fly.orbitDistance);
    controls.enabled = true;
    controls.update();
    setStatus('已退出自由飞行；当前位置保持不变，恢复环绕操作。');
  }
  updateFlyUI();
  requestRender();
}
function updateFly(delta) {
  if (!fly.enabled || !state.ready || state.loading || !flyMovementActive()) return;
  const forward = flyDirection();
  const right = new THREE.Vector3(Math.sin(fly.yaw), -Math.cos(fly.yaw), 0);
  const pressed = (code) => fly.keys.has(code) ? 1 : 0;
  const movement = forward.multiplyScalar(pressed('KeyW') - pressed('KeyS'))
    .addScaledVector(right, pressed('KeyD') - pressed('KeyA'))
    .addScaledVector(up, pressed('KeyE') - pressed('KeyQ'));
  if (movement.lengthSq() < 1e-10 || delta <= 0) return;
  const boost = fly.keys.has('ShiftLeft') || fly.keys.has('ShiftRight') ? 4 : 1;
  camera.position.addScaledVector(movement.normalize(), fly.speed * boost * delta);
  applyFlyLook();
}
function updateCameraReadout() {
  if (!state.data?.map) return;
  const direction = camera.getWorldDirection(new THREE.Vector3());
  const yaw = (THREE.MathUtils.radToDeg(Math.atan2(direction.y, direction.x)) + 360) % 360;
  const pitch = -THREE.MathUtils.radToDeg(Math.asin(THREE.MathUtils.clamp(direction.z, -1, 1)));
  $('camera-position').textContent = `XYZ  ${camera.position.toArray().map((value) => value.toFixed(1)).join(' / ')}`;
  $('camera-direction').textContent = `偏航 ${yaw.toFixed(1)}° / 俯仰 ${pitch.toFixed(1)}°（下为正）`;
}
function makeControls() {
  controls?.dispose();
  controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = false;
  controls.enabled = !fly.enabled;
  controls.screenSpacePanning = true;
  controls.minDistance = 12;
  controls.maxDistance = 150000;
  controls.minZoom = .02;
  controls.maxZoom = 120;
  controls.enableRotate = !state.topView;
  controls.mouseButtons.LEFT = state.topView ? THREE.MOUSE.PAN : THREE.MOUSE.ROTATE;
  controls.mouseButtons.MIDDLE = THREE.MOUSE.DOLLY;
  controls.mouseButtons.RIGHT = THREE.MOUSE.PAN;
  controls.addEventListener('change', requestRender);
}
function resize() {
  if (!renderer) return;
  const width = Math.max(1, $('canvas-host').clientWidth);
  const height = Math.max(1, $('canvas-host').clientHeight);
  renderer.setSize(width, height, false);
  const aspect = width / height;
  perspective.aspect = aspect;
  perspective.updateProjectionMatrix();
  orthographic.left = -state.orthoSpan * aspect / 2;
  orthographic.right = state.orthoSpan * aspect / 2;
  orthographic.top = state.orthoSpan / 2;
  orthographic.bottom = -state.orthoSpan / 2;
  orthographic.updateProjectionMatrix();
  requestRender();
}
function fitBox(box) {
  if (!box || !controls) return;
  if (fly.enabled) setFlyMode(false, false);
  const center = box.getCenter(new THREE.Vector3());
  const radius = Math.max(box.getSize(new THREE.Vector3()).length() / 2, 90);
  const halfFov = Math.min(THREE.MathUtils.degToRad(perspective.fov / 2),
    Math.atan(Math.tan(THREE.MathUtils.degToRad(perspective.fov / 2)) * perspective.aspect));
  const distance = radius / Math.sin(halfFov) * 1.13;
  if (state.topView) {
    camera.position.copy(center).add(new THREE.Vector3(0, -.01, Math.max(distance, 500)));
    state.orthoSpan = radius * 2.4;
    camera.zoom = 1;
    resize();
  } else {
    const direction = camera.position.clone().sub(controls.target);
    if (direction.lengthSq() < 1 || direction.z < .01) direction.set(1, -1, .85);
    camera.position.copy(center).addScaledVector(direction.normalize(), distance);
    camera.near = Math.max(.5, distance / 8000);
    camera.far = Math.max(100000, distance * 12);
    camera.updateProjectionMatrix();
  }
  controls.target.copy(center);
  controls.update();
  requestRender();
}
function fitWaypoints() { fitBox(state.waypointBounds || state.mapBounds); }
function setTopView(enabled) {
  if (fly.enabled) setFlyMode(false, false);
  if (enabled === state.topView || !controls) return;
  const target = controls.target.clone();
  if (enabled) {
    state.savedPerspective = { position: camera.position.clone(), target: target.clone() };
    const distance = Math.max(camera.position.distanceTo(target), 100);
    state.orthoSpan = distance * 2 * Math.tan(THREE.MathUtils.degToRad(perspective.fov / 2));
    orthographic.position.copy(target).add(new THREE.Vector3(0, -.01, distance));
    orthographic.zoom = 1;
    camera = orthographic;
  } else {
    camera = perspective;
    const direction = state.savedPerspective
      ? state.savedPerspective.position.clone().sub(state.savedPerspective.target).normalize()
      : new THREE.Vector3(1, -1, .85).normalize();
    const span = state.orthoSpan / orthographic.zoom;
    const distance = span / (2 * Math.tan(THREE.MathUtils.degToRad(perspective.fov / 2)));
    camera.position.copy(target).addScaledVector(direction, distance);
  }
  state.topView = enabled;
  makeControls();
  controls.target.copy(target);
  controls.update();
  $('view-mode').textContent = enabled ? '切换透视  T' : '切换俯视  T';
  $('view-mode').setAttribute('aria-pressed', String(enabled));
  $('mode-badge').textContent = enabled ? '正交俯视 · Z 向上' : '透视 · Z 向上';
  updateFlyUI();
  resize();
}
async function fetchGeometry(url, uvUrl = null, groups = null, warnings = [], label = url) {
  if (state.geometryCache.has(url)) {
    const cached = state.geometryCache.get(url);
    warnings.push(...(cached.userData.warnings || []));
    return cached;
  }
  const response = await fetch(url, { cache: 'no-cache' });
  if (!response.ok) throw new Error(`HTTP ${response.status}: ${url}`);
  const buffer = await response.arrayBuffer();
  if (buffer.byteLength % 36 !== 0) throw new Error(`Invalid triangle payload: ${url}`);
  const geometry = new THREE.BufferGeometry();
  const positions = new THREE.BufferAttribute(new Float32Array(buffer), 3);
  geometry.setAttribute('position', positions);
  const geometryWarnings = [];
  if (uvUrl) {
    try {
      const uvResponse = await fetch(uvUrl, { cache: 'no-cache' });
      if (!uvResponse.ok) throw new Error(`HTTP ${uvResponse.status}`);
      const uvBuffer = await uvResponse.arrayBuffer();
      if (uvBuffer.byteLength !== positions.count * 8) throw new Error('UV vertex count does not match positions');
      const coordinates = new Float32Array(uvBuffer);
      if (!coordinates.every(Number.isFinite)) throw new Error('UV contains non-finite coordinates');
      geometry.setAttribute('uv', new THREE.BufferAttribute(coordinates, 2));
    } catch (error) {
      geometryWarnings.push(`贴图坐标载入失败：${label}；已使用灰模。`);
      console.warn('Texture coordinates failed:', label, error);
    }
  }
  if (geometry.hasAttribute('uv') && Array.isArray(groups) && groups.length) {
    const sorted = groups.filter((group) => group.count !== 0).slice().sort((a, b) => a.start - b.start);
    let end = 0;
    const valid = sorted.every((group) => {
      if (!Number.isInteger(group.start) || !Number.isInteger(group.count) || group.start !== end
        || group.count < 0 || group.start % 3 || group.count % 3 || group.start + group.count > positions.count) return false;
      end = group.start + group.count;
      return true;
    }) && end === positions.count;
    if (valid) {
      const byMaterial = new Map();
      for (const group of sorted) {
        const id = Number.isInteger(group.material) && group.material >= 0 ? group.material : null;
        if (!byMaterial.has(id)) byMaterial.set(id, []);
        byMaterial.get(id).push(group);
      }
      // Batch separated runs of one material using indices; retain original XYZ/UV order.
      const indices = positions.count > 65535 ? new Uint32Array(positions.count) : new Uint16Array(positions.count);
      let cursor = 0;
      const ids = [];
      for (const [id, runs] of byMaterial) {
        const start = cursor;
        for (const run of runs) {
          for (let vertex = run.start; vertex < run.start + run.count; vertex++) indices[cursor++] = vertex;
        }
        geometry.addGroup(start, cursor - start, ids.length);
        ids.push(id);
      }
      geometry.setIndex(new THREE.BufferAttribute(indices, 1));
      geometry.userData.materialIds = ids;
    } else geometryWarnings.push(`材质分组无效：${label}；已使用灰模。`);
  }
  geometry.computeVertexNormals();
  geometry.computeBoundingBox();
  geometry.computeBoundingSphere();
  geometry.userData.cached = true;
  geometry.userData.warnings = geometryWarnings;
  warnings.push(...geometryWarnings);
  state.geometryCache.set(url, geometry);
  return geometry;
}
async function fetchTexture(row) {
  if (state.textureCache.has(row.url)) return state.textureCache.get(row.url);
  const texture = await new THREE.TextureLoader().loadAsync(row.url);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  // Source BSP/VVD V=0 addresses the source image's top row. Do not invert it.
  texture.flipY = false;
  texture.needsUpdate = true;
  state.textureCache.set(row.url, texture);
  return texture;
}
async function loadTextures(data, warnings) {
  const textures = new Map();
  const rows = data.textures || [];
  let next = 0, completed = 0;
  const worker = async () => {
    while (next < rows.length) {
      const row = rows[next++];
      try { textures.set(row.id, await fetchTexture(row)); }
      catch (error) {
        const names = (data.materials || []).filter((material) => material.texture_id === row.id).map((material) => material.name);
        warnings.push(`基础贴图载入失败：${names.slice(0, 3).join('、') || `贴图 #${row.id}`}；对应材质已使用灰模。`);
        console.warn('Base texture failed:', row.url, error);
      }
      completed++;
      $('loading-text').textContent = `正在载入基础贴图 ${completed} / ${rows.length}…`;
    }
  };
  await Promise.all(Array.from({ length: Math.min(6, rows.length) }, worker));
  for (const material of data.materials || []) {
    if (material.texture_id !== null && material.texture_id !== undefined && !rows.some((row) => row.id === material.texture_id)) {
      warnings.push(`基础贴图缺少资源：${material.name}；已使用灰模。`);
    }
  }
  return textures;
}
function buildProps(data, geometries, warnings) {
  const modelGroup = new THREE.Group();
  modelGroup.name = 'map-props';
  state.models = modelGroup;
  state.root.add(modelGroup);
  const instances = new Map();
  const missing = [];
  for (const prop of data.props || []) {
    if (prop.model_id !== null && prop.model_id !== undefined && geometries.has(prop.model_id)) {
      if (!instances.has(prop.model_id)) instances.set(prop.model_id, []);
      instances.get(prop.model_id).push(prop);
    } else missing.push(prop);
  }
  let renderedProps = 0;
  for (const [id, props] of instances) {
    const mesh = new THREE.InstancedMesh(geometries.get(id), surfaceMaterials(geometries.get(id), '#89949f', warnings), props.length);
    mesh.name = `prop-model-${id}`;
    props.forEach((prop, index) => {
      mesh.setMatrixAt(index, sourceAngleMatrix(prop.origin, prop.angles, prop.scale));
    });
    mesh.instanceMatrix.needsUpdate = true;
    mesh.computeBoundingSphere();
    modelGroup.add(mesh);
    renderedProps += props.length;
  }
  const missingMaterial = new THREE.LineBasicMaterial({
    color: '#ffa544', depthTest: false, transparent: true, opacity: .95, clippingPlanes: [clipPlane],
  });
  const markerMaterial = new THREE.MeshBasicMaterial({
    color: '#ffa544', depthTest: false, transparent: true, opacity: .9, clippingPlanes: [clipPlane],
  });
  const smallMarkers = [];
  for (const prop of missing) {
    const row = (data.models || []).find((model) => model.id === prop.model_id);
    const bounds = boxFromBounds(prop.bounds || row?.bounds);
    if (bounds) {
      const size = bounds.getSize(new THREE.Vector3());
      const center = bounds.getCenter(new THREE.Vector3());
      const geometry = new THREE.BoxGeometry(size.x, size.y, size.z).translate(center.x, center.y, center.z);
      const edges = new THREE.EdgesGeometry(geometry);
      geometry.dispose();
      const outline = new THREE.LineSegments(edges, missingMaterial);
      outline.matrixAutoUpdate = false;
      outline.matrix.copy(sourceAngleMatrix(prop.origin, prop.angles, prop.scale));
      outline.renderOrder = 6;
      modelGroup.add(outline);
    } else smallMarkers.push(prop.origin);
  }
  if (smallMarkers.length) {
    // Small diamonds indicate resource origins, never invented bounding boxes.
    const markers = new THREE.InstancedMesh(new THREE.OctahedronGeometry(10), markerMaterial, smallMarkers.length);
    const transform = new THREE.Matrix4();
    smallMarkers.forEach((origin, index) => markers.setMatrixAt(index, transform.makeTranslation(...origin)));
    markers.instanceMatrix.needsUpdate = true;
    markers.computeBoundingSphere();
    markers.renderOrder = 6;
    markers.name = 'missing-model-location-markers';
    modelGroup.add(markers);
  }
  if (!missing.length) { missingMaterial.dispose(); markerMaterial.dispose(); }
  else {
    if (!smallMarkers.length) markerMaterial.dispose();
    if (smallMarkers.length === missing.length) missingMaterial.dispose();
    const unique = [...new Set(missing.map((prop) => prop.model || '(unnamed)'))];
    warnings.unshift(`有 ${missing.length} 个物件未能显示几何（${unique.length} 种模型）。橙色菱形仅表示资源原点，不代表实际大小。`);
    for (const name of unique) {
      const props = missing.filter((prop) => (prop.model || '(unnamed)') === name);
      const origin = props[0].origin.map((value) => Number(value.toFixed(1))).join(', ');
      warnings.push(`缺失物件：${name} × ${props.length}；首个位置 (${origin})。`);
    }
  }
  state.stats.loadedProps = renderedProps;
  state.stats.missingProps = missing.length;
  modelGroup.visible = $('show-models').checked;
}
function addOverlayRings(points, root) {
  for (const [flagName, color] of NODE_OVERLAYS) {
    const indices = points.map((point, index) => point.flags & FLAG_BITS[flagName] ? index : -1).filter((index) => index >= 0);
    const rows = indices.map((index) => points[index]);
    if (!rows.length) continue;
    const geometry = new THREE.TorusGeometry(9.5 + NODE_OVERLAYS.findIndex(([name]) => name === flagName) * 2.7, 1.4, 5, 16);
    const material = waypointMaterial(THREE.MeshBasicMaterial, { color, depthWrite: false });
    const mesh = new THREE.InstancedMesh(geometry, material, rows.length);
    mesh.renderOrder = 10;
    const matrix = new THREE.Matrix4();
    rows.forEach((point, index) => mesh.setMatrixAt(index, matrix.makeTranslation(...point.origin)));
    mesh.instanceMatrix.needsUpdate = true;
    mesh.computeBoundingSphere();
    root.add(mesh);
    state.markerOverlays.push({ mesh, indices });
  }
}
function buildWaypoints(data) {
  const points = data.waypoints?.points || [];
  state.points = points;
  state.markerScales = new Float32Array(points.length);
  state.markerOverlays = [];
  state.arrowSets = [];
  state.incoming = points.map(() => []);
  const waypointRoot = new THREE.Group();
  waypointRoot.name = 'waypoints';
  state.root.add(waypointRoot);
  const bounds = new THREE.Box3();
  for (const point of points) bounds.expandByPoint(new THREE.Vector3(...point.origin));
  state.waypointBounds = points.length ? bounds : null;
  if (!points.length) {
    state.nodes = null;
    state.stats.waypoints = 0;
    state.stats.directedLinks = 0;
    return;
  }
  const nodes = new THREE.InstancedMesh(
    new THREE.SphereGeometry(6.5, 10, 7),
    waypointMaterial(THREE.MeshBasicMaterial, { depthWrite: false }), points.length,
  );
  nodes.name = 'waypoint-nodes';
  nodes.renderOrder = 12;
  const matrix = new THREE.Matrix4();
  const color = new THREE.Color();
  points.forEach((point, index) => {
    nodes.setMatrixAt(index, matrix.makeTranslation(...point.origin));
    nodes.setColorAt(index, color.set(nodeColor(point.flags)));
  });
  nodes.instanceMatrix.needsUpdate = true;
  nodes.instanceColor.needsUpdate = true;
  nodes.computeBoundingSphere();
  state.nodes = nodes;
  waypointRoot.add(nodes);
  addOverlayRings(points, waypointRoot);
  const connectionSet = new Set();
  points.forEach((point, index) => {
    for (const [target] of point.links || []) {
      if (Number.isInteger(target) && target >= 0 && target < points.length && target !== index) {
        connectionSet.add(`${index}:${target}`);
      }
    }
  });
  const buckets = EDGE_COLORS.map(() => ({ segments: [], arrows: [] }));
  let directed = 0;
  points.forEach((point, index) => {
    for (const [target, flags] of point.links || []) {
      if (!Number.isInteger(target) || target < 0 || target >= points.length || target === index) continue;
      state.incoming[target].push([index, flags]);
      const start = new THREE.Vector3(...point.origin);
      const end = new THREE.Vector3(...points[target].origin);
      const direction = end.clone().sub(start);
      const length = direction.length();
      if (length < .1) continue;
      direction.divideScalar(length);
      if (connectionSet.has(`${target}:${index}`)) {
        const side = direction.clone().cross(up);
        if (side.lengthSq() < .001) side.crossVectors(direction, new THREE.Vector3(1, 0, 0));
        side.normalize().multiplyScalar(3.5);
        start.add(side); end.add(side);
      }
      const a = start.clone().addScaledVector(direction, Math.min(9, length * .12));
      const b = end.clone().addScaledVector(direction, -Math.min(10, length * .15));
      const bucket = buckets[edgeClass(flags)];
      bucket.segments.push(...a.toArray(), ...b.toArray());
      // Each direction gets its own arrow, even when two-way flags agree.
      const arrowLength = Math.min(15, Math.max(4, length * .2));
      const arrowCenter = a.clone().lerp(b, .64);
      const quaternion = new THREE.Quaternion().setFromUnitVectors(yAxis, direction);
      bucket.arrows.push({ center: arrowCenter, quaternion, length: arrowLength, linkLength: length });
      directed++;
    }
  });
  buckets.forEach((bucket, index) => {
    if (!bucket.arrows.length) return;
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.Float32BufferAttribute(bucket.segments, 3));
    const lines = new THREE.LineSegments(geometry,
      waypointMaterial(THREE.LineBasicMaterial, { color: EDGE_COLORS[index], transparent: true, opacity: .82, depthWrite: false }));
    lines.renderOrder = 8;
    lines.name = `directed-links-${index}`;
    waypointRoot.add(lines);
    const arrows = new THREE.InstancedMesh(new THREE.ConeGeometry(3.8, 15, 5),
      waypointMaterial(THREE.MeshBasicMaterial, { color: EDGE_COLORS[index], depthWrite: false }), bucket.arrows.length);
    bucket.arrows.forEach((arrow, arrowIndex) => {
      arrows.setMatrixAt(arrowIndex, new THREE.Matrix4().compose(arrow.center, arrow.quaternion, new THREE.Vector3(1, arrow.length / 15, 1)));
    });
    state.arrowSets.push({ mesh: arrows, arrows: bucket.arrows });
    arrows.instanceMatrix.needsUpdate = true;
    arrows.computeBoundingSphere();
    arrows.renderOrder = 9;
    arrows.name = `direction-arrows-${index}`;
    waypointRoot.add(arrows);
  });
  const selected = new THREE.Mesh(new THREE.SphereGeometry(11, 14, 10),
    waypointMaterial(THREE.MeshBasicMaterial, { color: '#ffdc83', wireframe: true, depthWrite: false }));
  selected.visible = false;
  selected.renderOrder = 15;
  selected.name = 'selected-waypoint';
  state.selection = selected;
  waypointRoot.add(selected);
  const radiusCircle = new THREE.Mesh(new THREE.TorusGeometry(1, .012, 5, 64),
    waypointMaterial(THREE.MeshBasicMaterial, { color: '#ffd88b', opacity: .6, depthWrite: false, side: THREE.DoubleSide }));
  radiusCircle.visible = false;
  radiusCircle.renderOrder = 14;
  radiusCircle.name = 'selected-waypoint-radius';
  state.selectionRadius = radiusCircle;
  waypointRoot.add(radiusCircle);
  state.labels = points.map((point, index) => {
    const label = element('span', String(index), 'point-label');
    label.hidden = true;
    $('labels').append(label);
    return label;
  });
  state.stats.waypoints = points.length;
  state.stats.directedLinks = directed;
}
function editorMessage(message, error = false) {
  $('editor-message').textContent = message;
  $('editor-message').classList.toggle('error', error);
}
function updateEditorUI() {
  const editor = state.editor;
  const busy = state.loading || state.saving;
  $('save-waypoints').disabled = !editor || busy;
  $('save-as').disabled = !editor || busy;
  $('download-copy').disabled = !editor || busy;
  $('undo-edit').disabled = !editor?.canUndo || busy;
  $('redo-edit').disabled = !editor?.canRedo || busy;
  $('document-state').textContent = state.saving ? '正在保存…' : !editor ? '未载入编辑文档' : editor.dirty ? '有未保存修改' : '未修改';
  $('document-state').classList.toggle('dirty', !!editor?.dirty);
  document.querySelectorAll('#waypoint-edit-form input, #waypoint-edit-form button').forEach((input) => { input.disabled = busy; });
  if (state.data?.map) document.title = `${editor?.dirty ? '* ' : ''}${state.data.map.name} · AgPW 3D 路点编辑`;
}
function setSaving(busy) {
  state.saving = busy;
  if (busy) suspendFly();
  const unavailable = busy || state.loading;
  $('import-agpw').disabled = unavailable;
  $('import-file').disabled = unavailable;
  $('empty-import').disabled = unavailable;
  $('reload').disabled = unavailable || !state.data || !!state.data.empty;
  updateEditorUI();
}
function waypointFilename() {
  const name = (state.data?.waypoints?.path || '').split(/[\\/]/).pop();
  return name?.toLowerCase().endsWith('.agpw') ? name : `${state.data?.waypoints?.map_name || 'waypoints'}.agpw`;
}
function downloadWaypointCopy(blob, name) {
  const url = URL.createObjectURL(blob);
  const anchor = element('a'); anchor.href = url; anchor.download = name;
  document.body.append(anchor); anchor.click(); anchor.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}
async function saveWaypoints(saveAs = false, downloadOnly = false) {
  const editor = state.editor;
  if (!editor || state.loading || state.saving) return false;
  const revision = editor.revision;
  const changes = editor.getChanges();
  const sceneId = state.data.scene_id;
  let handle = downloadOnly || saveAs ? null : state.saveHandle;
  let picker = null;
  setSaving(true);
  try {
    if (!downloadOnly && !handle && typeof window.showSaveFilePicker === 'function') {
      try {
        // Request the native target in this click's activation before any await/export.
        picker = window.showSaveFilePicker({ suggestedName: waypointFilename(),
          types: [{ description: 'AgPW 路点文件', accept: { 'application/octet-stream': ['.agpw'] } }] });
      } catch (error) {
        if (error.name === 'AbortError') { editorMessage('已取消保存；修改仍未保存。'); return false; }
        console.warn('Save file picker unavailable:', error);
      }
      if (picker) {
        try { handle = await picker; }
        catch (error) {
          if (error.name === 'AbortError') { editorMessage('已取消保存；修改仍未保存。'); return false; }
          console.warn('Save file picker denied:', error);
          handle = null;
        }
      }
    }
    editorMessage(handle ? '正在导出并写入选择的文件…' : downloadOnly ? '正在导出下载副本…' : '浏览器未提供文件写入，将导出下载副本。');
    const response = await fetch('export.agpw', { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ scene_id: sceneId, source_hash: editor.sourceHash, changes }) });
    if (!response.ok) {
      let reason = `HTTP ${response.status}`;
      try { reason = (await response.json()).error || reason; } catch {}
      throw new Error(reason);
    }
    const blob = await response.blob();
    if (handle) {
      let writable;
      try {
        writable = await handle.createWritable();
        await writable.write(blob);
        await writable.close();
      } catch (error) {
        if (writable) { try { await writable.abort(); } catch {} }
        throw new Error('未能写入选择的文件，请用“另存为”重新选择目标。');
      }
      state.saveHandle = handle;
      updateSourceNote(state.data);
      showMapInfo(state.data);
      const saved = state.editor === editor && editor.markSaved(revision);
      const message = saved ? `已保存到 ${handle.name}。` : '文件已写入；后续修改仍未保存。';
      editorMessage(message); setStatus(message);
    } else {
      downloadWaypointCopy(blob, waypointFilename());
      const message = '已导出副本，原文件未修改；修改仍未标记为已保存。';
      editorMessage(message); setStatus(message);
    }
    return true;
  } catch (error) {
    editorMessage(`保存失败：${error.message}`, true);
    setStatus(`保存失败：${error.message}`);
    console.error('Waypoint save failed:', error);
    return false;
  } finally { setSaving(false); }
}
function confirmDiscardEdits() {
  return !state.editor?.dirty || window.confirm('当前路点有未保存的修改。继续将丢弃这些修改，是否继续？');
}
function initializeEditor(data, saveHandle = null) {
  state.saveHandle = saveHandle;
  state.editor = data.editor?.source_hash
    ? new WaypointEditor(data.waypoints.points, data.editor, documentChanged) : null;
  editorMessage(state.editor ? '选择路点，修改后点击“应用修改”。保存时由你选择目标文件。' : '当前场景未提供编辑信息，仅支持预览。');
  updateEditorUI();
}
function rebuildWaypointOverlay() {
  const selected = state.selected;
  const oldOverlay = state.root?.getObjectByName('waypoints');
  disposeTree(oldOverlay);
  $('labels').replaceChildren();
  state.waypointMaterials = [];
  state.labels = [];
  state.selection = null;
  state.selectionRadius = null;
  buildWaypoints(state.data);
  if (selected >= 0 && selected < state.points.length) selectWaypoint(selected);
  else clearSelection();
  requestRender();
}
function documentChanged(event) {
  if (event.index !== null) {
    Object.assign(state.data.waypoints.points[event.index], state.editor.getPoint(event.index));
    rebuildWaypointOverlay();
    const action = event.kind === 'undo' ? '已撤销' : event.kind === 'redo' ? '已重做' : '已应用修改';
    editorMessage(`${action}：路点 #${event.index}。`);
    setStatus(`${action}：路点 #${event.index}${state.editor.dirty ? '；尚未保存。' : '。'}`);
  }
  updateEditorUI();
}
function appendWaypointEditor(parent, index) {
  if (!state.editor) return;
  const editor = state.editor;
  const point = editor.getPoint(index);
  const form = element('form', undefined, 'waypoint-edit');
  form.id = 'waypoint-edit-form';
  form.append(element('h3', '编辑半径与 Flags'));
  for (const field of editor.fields.filter((field) => field.key !== 'flags')) {
    const label = element('label', undefined, 'edit-number');
    label.append(element('span', field.label || field.key));
    const input = element('input');
    input.type = 'number'; input.step = '1'; input.required = true;
    input.min = String(field.min ?? 0); input.max = String(field.max ?? 255);
    input.value = String(point[field.key]); input.dataset.editorField = field.key;
    input.id = `edit-${field.key}`;
    label.append(input); form.append(label);
  }
  if (editor.fields.some((field) => field.key === 'flags')) {
    const flags = element('fieldset', undefined, 'edit-flags');
    flags.id = 'edit-flags'; flags.append(element('legend', 'Flags'));
    for (const flag of editor.flags) {
      const label = element('label', undefined, 'edit-flag');
      const checkbox = element('input'); checkbox.type = 'checkbox';
      checkbox.checked = !!(point.flags & flag.bit); checkbox.dataset.flagBit = String(flag.bit);
      checkbox.setAttribute('aria-label', `${flag.label || flag.name} (${flag.name})`);
      label.title = `${flag.name} · 0x${flag.bit.toString(16).toUpperCase()}`;
      label.append(checkbox, element('span', flag.label || flag.name)); flags.append(label);
    }
    form.append(flags);
    const unknown = editor.unknownFlags(index);
    const note = element('p', unknown ? `未知属性位 0x${unknown.toString(16).toUpperCase()}：保留，不修改。` : '未知属性位：无。', 'unknown-flags');
    note.id = 'unknown-flags'; form.append(note);
  }
  const apply = element('button', '应用修改', 'wide'); apply.type = 'submit'; apply.id = 'apply-waypoint';
  const message = element('p', '位置、连线及其他字段保持只读。', 'edit-message'); message.id = 'edit-message';
  form.append(apply, message);
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    if (state.loading || state.saving || state.editor !== editor) return;
    const updates = {};
    for (const input of form.querySelectorAll('[data-editor-field]')) updates[input.dataset.editorField] = input.value;
    if (editor.fields.some((field) => field.key === 'flags')) {
      let known = 0;
      for (const input of form.querySelectorAll('[data-flag-bit]')) if (input.checked) known = (known | Number(input.dataset.flagBit)) >>> 0;
      updates.flags = editor.mergeFlags(index, known);
    }
    try {
      if (!editor.apply(index, updates)) { message.textContent = '数值未变化。'; message.classList.remove('error'); }
    } catch (error) { message.textContent = error.message; message.classList.add('error'); }
  });
  parent.append(form);
}
function showEdgeList(parent, title, links, incoming = false) {
  parent.append(element('h3', `${title} (${links.length})`, 'link-heading'));
  const list = element('div', undefined, 'edge-list');
  if (!links.length) list.append(element('span', '无', 'hint'));
  for (const [target, flags] of links) {
    const row = element('div', undefined, 'edge-row');
    row.style.setProperty('--edge', EDGE_COLORS[edgeClass(flags)]);
    const valid = Number.isInteger(target) && target >= 0 && target < state.points.length;
    const button = element('button', `${incoming ? '←' : '→'} ${target}`);
    button.type = 'button';
    button.disabled = !valid;
    if (valid) button.addEventListener('click', () => selectWaypoint(target, true));
    row.append(button, element('span', flagsText(flags, PATH_FLAGS).join(' | ')));
    if (!valid) row.append(element('span', '无效引用'));
    list.append(row);
  }
  parent.append(list);
}
function clearSelection() {
  state.selected = -1;
  if (state.selection) state.selection.visible = false;
  if (state.selectionRadius) state.selectionRadius.visible = false;
  $('selection').replaceChildren(element('h2', '路点详情'));
  const empty = element('div', undefined, 'empty-selection');
  empty.append(element('span', '⌖'), element('p', '点击地图中的路点，查看坐标、属性与方向连接'));
  $('selection').append(empty);
  state.labels.forEach((label) => label.classList.remove('selected'));
  requestRender();
}
function selectWaypoint(index, focus = false) {
  if (!Number.isInteger(index) || index < 0 || index >= state.points.length) return false;
  state.selected = index;
  const point = state.points[index];
  if (state.selection) {
    state.selection.position.set(...point.origin);
    state.selection.visible = true;
  }
  if (state.selectionRadius) {
    state.selectionRadius.position.set(...point.origin);
    state.selectionRadius.scale.set(point.radius, point.radius, 1);
    state.selectionRadius.visible = point.radius > 0;
  }
  const panel = $('selection');
  panel.replaceChildren();
  const heading = element('div', undefined, 'selection-heading');
  heading.append(element('h2', `路点 #${index}`));
  const focusButton = element('button', '聚焦');
  focusButton.addEventListener('click', () => focusWaypoint(index));
  heading.append(focusButton);
  panel.append(heading);
  const list = descriptionList(panel, [
    ['坐标 XYZ', point.origin.map((value) => value.toFixed(2)).join(', ')],
    ['半径 / Mesh', `${point.radius} / ${point.mesh}`],
    ['重力', Number(point.gravity).toFixed(3)],
    ['属性位', `0x${(point.flags >>> 0).toString(16).toUpperCase().padStart(8, '0')}`],
  ]);
  list.append(element('dt', '路点属性'));
  const flagContainer = element('dd');
  addFlags(flagContainer, point.flags, WP_FLAGS);
  list.append(flagContainer);
  appendWaypointEditor(panel, index);
  showEdgeList(panel, '出边', point.links || []);
  showEdgeList(panel, '入边', state.incoming[index] || [], true);
  state.labels.forEach((label, i) => label.classList.toggle('selected', i === index));
  $('locate-id').value = String(index);
  $('locate-message').textContent = '';
  if (focus) focusWaypoint(index);
  setStatus(`已选择路点 #${index} · ${point.origin.map((value) => value.toFixed(1)).join(', ')}`);
  updateEditorUI();
  requestRender();
  return true;
}
function focusWaypoint(index) {
  const point = state.points[index];
  if (!point || !controls) return;
  if (fly.enabled) setFlyMode(false, false);
  if (point.origin[2] > Number($('slice').value)) {
    $('slice').value = String(Math.min(state.sliceMax, point.origin[2] + 40));
    applySlice();
  }
  const target = new THREE.Vector3(...point.origin);
  const direction = camera.position.clone().sub(controls.target);
  if (state.topView) {
    camera.position.copy(target).add(new THREE.Vector3(0, -.01, 900));
    state.orthoSpan = 550;
    camera.zoom = 1;
    resize();
  } else {
    if (direction.lengthSq() < 1) direction.set(1, -1, .85);
    camera.position.copy(target).addScaledVector(direction.normalize(), 430);
  }
  controls.target.copy(target);
  controls.update();
  requestRender();
}
function applySlice() {
  const limit = Number($('slice').value);
  const full = limit >= state.sliceMax;
  clipPlane.constant = full ? 1e10 : limit;
  $('slice-value').textContent = full ? '全部' : `${Math.round(limit)}`;
  $('slice-badge').hidden = full;
  $('slice-badge').textContent = `切片 Z ≤ ${Math.round(limit)}`;
  requestRender();
}
function applyOpacity() {
  const opacity = Number($('opacity').value) / 100;
  $('opacity-value').textContent = `${Math.round(opacity * 100)}%`;
  state.mapMaterials.forEach((material) => {
    material.opacity = opacity;
    material.transparent = opacity < 1;
    material.depthWrite = true;
    material.needsUpdate = true;
  });
  requestRender();
}
function applyXray() {
  state.waypointMaterials.forEach((material) => {
    material.depthTest = !$('xray').checked;
    material.needsUpdate = true;
  });
  requestRender();
}
function visibleInHierarchy(object) {
  for (let current = object; current; current = current.parent) if (!current.visible) return false;
  return true;
}
function mapOccludes(hits) {
  return hits.some((hit) => hit.point.z <= clipPlane.constant && visibleInHierarchy(hit.object)
    && hit.object.material?.depthTest !== false);
}
function updateLabels() {
  const showAll = $('show-labels').checked;
  const width = $('labels').clientWidth, height = $('labels').clientHeight;
  for (let index = 0; index < state.labels.length; index++) {
    const label = state.labels[index];
    const origin = state.points[index].origin;
    if ((!showAll && state.selected !== index) || origin[2] > clipPlane.constant) {
      label.hidden = true;
      continue;
    }
    projected.set(...origin).project(camera);
    if (projected.z < -1 || projected.z > 1 || Math.abs(projected.x) > 1.05 || Math.abs(projected.y) > 1.05) {
      label.hidden = true;
      continue;
    }
    if (!$('xray').checked) {
      const destination = new THREE.Vector3(...origin);
      const cameraPoint = camera.position.clone();
      if (camera.isOrthographicCamera) {
        cameraPoint.copy(destination).addScaledVector(camera.getWorldDirection(new THREE.Vector3()), -camera.position.distanceTo(controls.target));
      }
      const delta = destination.clone().sub(cameraPoint);
      const distance = delta.length();
      raycaster.set(cameraPoint, delta.normalize());
      raycaster.far = Math.max(0, distance - 9);
      const hits = raycaster.intersectObjects(state.root.children.filter((child) => child.name !== 'waypoints'), true);
      const blocked = mapOccludes(hits);
      raycaster.far = Infinity;
      if (blocked) { label.hidden = true; continue; }
    }
    label.hidden = false;
    label.style.left = `${(projected.x * .5 + .5) * width}px`;
    label.style.top = `${(-projected.y * .5 + .5) * height}px`;
  }
  state.labelNeeded = false;
}
function showWarnings(messages) {
  const unique = [...new Set(messages.filter(Boolean))];
  const list = $('warnings');
  list.replaceChildren();
  for (const warning of unique) list.append(element('li', warning));
  if (!unique.length) list.append(element('li', '地图和模型资源均已读取，未发现额外提示。'));
  $('warning-count').textContent = String(unique.length);
  $('warning-count').classList.toggle('warn', !!unique.length);
  state.stats.warnings = unique.length;
}
function showMapInfo(data) {
  const stats = data.map.stats || {};
  const info = $('map-info');
  info.replaceChildren();
  for (const [name, value] of [
    ['地图', `${data.map.name} · BSP v${data.map.version}`],
    ['路点文件', data.waypoints?.path || '—'],
    ['文件来源', state.saveHandle ? `保存文件：${state.saveHandle.name}；重新加载读取该文件`
      : state.imported ? '导入快照；文件修改后需重新导入' : '磁盘路径；重新加载可读取文件修改'],
    ['路点地图 / 作者', `${data.waypoints?.map_name || '—'} / ${data.waypoints?.author || '—'}`],
    ['地图三角形', (data.world?.triangles || 0).toLocaleString()],
    ['物件', `${state.stats.loadedProps} / ${(data.props || []).length} 已显示`],
    ['模型资源', `${(data.models || []).length} 种；缺失 ${stats.missing_models ?? state.stats.missingProps} 种`],
    ['基础贴图', `${state.stats.loadedTextures || 0} / ${state.stats.textureCount || 0} 已载入`],
  ]) info.append(element('dt', name), element('dd', value));
}
function setBusy(busy) {
  if (busy) {
    if (fly.enabled) setFlyMode(false, false);
    resetFlyInput();
  }
  state.loading = busy;
  const unavailable = busy || state.saving;
  $('import-agpw').disabled = unavailable;
  $('import-file').disabled = unavailable;
  $('empty-import').disabled = unavailable;
  $('reload').disabled = unavailable || !state.data || !!state.data.empty;
  updateFlyUI();
  updateEditorUI();
}
function updateSourceNote(data) {
  const source = data.source_kind || data.waypoints?.source_kind;
  if (source) state.imported = source === 'upload';
  if (state.saveHandle) {
    $('reload').textContent = '重新读取保存文件';
    $('reload').title = `重新读取已保存文件 ${state.saveHandle.name}`;
    $('source-note').textContent = `当前保存目标：${state.saveHandle.name}。“重新读取保存文件”会从这个文件读取路点，保存也会写入这个目标。`;
    return;
  }
  $('reload').textContent = state.imported ? '重载导入快照' : '重新读取路点';
  $('reload').title = state.imported
    ? '重新加载上次导入的快照；原文件修改后请再次导入'
    : '重新读取磁盘上的路点；地图与模型资源变更后请重启启动器';
  $('source-note').textContent = state.imported
    ? '当前显示上次导入的文件快照。“重新加载”仍读取该快照；原文件修改后，请再次点击“导入 AGPW”。'
    : '“重新加载”会重新读取当前磁盘路径下的路点。也可点击“导入 AGPW”切换文件；工具按文件中的地图名寻找对应地图。';
}
function showEmptyScene(data) {
  disposeTree(state.root);
  for (const geometry of state.geometryCache.values()) geometry.dispose();
  state.geometryCache.clear();
  for (const texture of state.textureCache.values()) texture.dispose();
  state.textureCache.clear();
  state.materialCache.clear();
  state.materialDefinitions.clear();
  state.loadedTextures.clear();
  state.data = data;
  state.root = null;
  state.nodes = null;
  state.selection = null;
  state.selectionRadius = null;
  state.editor = null;
  state.saveHandle = null;
  state.models = null;
  state.points = [];
  state.incoming = [];
  state.markerOverlays = [];
  state.arrowSets = [];
  state.labels = [];
  state.mapBounds = null;
  state.waypointBounds = null;
  state.mapMaterials = [];
  state.waypointMaterials = [];
  state.stats = { empty: true, waypoints: 0, directedLinks: 0 };
  $('labels').replaceChildren();
  clearSelection();
  $('map-info').replaceChildren();
  $('warnings').replaceChildren();
  $('warning-count').textContent = '0';
  $('warning-count').classList.remove('warn');
  $('scene-name').textContent = '尚未导入路点文件';
  $('scene-stats').textContent = '等待导入 AGPW';
  $('mode-badge').hidden = true;
  $('slice-badge').hidden = true;
  $('empty-state').hidden = false;
  $('camera-readout').hidden = true;
  document.title = 'AgPW 3D 路点预览';
  state.ready = true;
  setStatus('请选择 AGPW 路点文件；工具会按文件中的地图名查找 BSP。');
  requestRender();
}
async function responseManifest(response) {
  const text = await response.text();
  let body;
  try { body = JSON.parse(text); }
  catch { throw new Error(`服务器返回了无效响应（HTTP ${response.status}）`); }
  if (!response.ok) throw new Error(body.error || `请求失败（HTTP ${response.status}）`);
  return body;
}
function showImportFailure(error, filename) {
  state.error = error.message;
  $('fatal').hidden = true;
  $('import-error-text').textContent = `${filename}：${error.message}`;
  $('import-error-note').textContent = state.data?.map
    ? `当前仍显示上一次成功载入的场景「${state.data.map.name}」，本次导入未成功。`
    : '本次文件未载入，请检查错误后重新导入。';
  $('import-error').hidden = false;
  $('empty-state').hidden = !state.data?.empty && !!state.data;
  setStatus(`导入失败：${error.message}`);
  console.error('Waypoint import failed:', error);
}
async function reloadWaypoints() {
  if (!state.saveHandle) return loadScene();
  if (state.loading || state.saving || !confirmDiscardEdits()) return false;
  const handle = state.saveHandle;
  setBusy(true);
  try {
    const file = await handle.getFile();
    return await importWaypoint(file, { alreadyBusy: true, discardConfirmed: true, saveHandleAfterImport: handle });
  } catch (error) {
    const message = '无法读取保存文件；当前路点仍然保留，请检查文件访问权限。';
    editorMessage(message, true); setStatus(message);
    console.error('Saved waypoint file reload failed:', error);
    return false;
  } finally { setBusy(false); }
}
async function importWaypoint(file, options = {}) {
  if (!file || (state.loading && !options.alreadyBusy) || state.saving) return false;
  if (!options.discardConfirmed && !confirmDiscardEdits()) { $('import-file').value = ''; return false; }
  if (!file.name.toLowerCase().endsWith('.agpw')) {
    showImportFailure(new Error('请选择 .agpw 路点文件。'), file.name);
    $('import-file').value = '';
    return false;
  }
  const wasReady = state.ready;
  setBusy(true);
  state.error = null;
  $('import-error').hidden = true;
  $('fatal').hidden = true;
  $('empty-state').hidden = true;
  $('loading').hidden = false;
  $('loading-text').textContent = `正在导入 ${file.name}…`;
  setStatus(`正在导入 ${file.name} 并查找对应地图…`);
  try {
    const buffer = await file.arrayBuffer();
    const response = await fetch('import.agpw', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'X-AGPW-Name': encodeURIComponent(file.name) },
      body: buffer,
    });
    const manifest = await responseManifest(response);
    return await loadScene(manifest, { fromImport: true, filename: file.name, saveHandleAfterImport: options.saveHandleAfterImport || null });
  } catch (error) {
    state.ready = wasReady;
    showImportFailure(error, file.name);
    return false;
  } finally {
    setBusy(false);
    $('loading').hidden = true;
    $('import-file').value = '';
  }
}
async function loadScene(manifest = null, options = {}) {
  const fromImport = options.fromImport === true;
  if (state.saving || (state.loading && !fromImport)) return false;
  if (!fromImport && !confirmDiscardEdits()) return false;
  const wasReady = state.ready;
  const oldCache = state.geometryCache;
  const oldTextureCache = state.textureCache;
  let cacheReplaced = false;
  let sceneCommitted = false;
  setBusy(true);
  state.error = null;
  $('loading').hidden = false;
  $('fatal').hidden = true;
  $('import-error').hidden = true;
  $('empty-state').hidden = true;
  $('loading-text').textContent = '正在读取场景…';
  setStatus('正在读取地图和路点…');
  try {
    const data = manifest || await responseManifest(await fetch('scene.json', { cache: 'no-store' }));
    if (data.empty) {
      showEmptyScene(data);
      return true;
    }
    if (!data.map || !data.world || !data.waypoints || !Array.isArray(data.waypoints.points)) {
      throw new Error('Invalid scene manifest');
    }
    const warnings = [...(data.warnings || []), ...(data.map.warnings || []), ...(data.waypoint_warnings || []), ...(data.waypoints.warnings || [])];
    $('loading-text').textContent = '正在载入地图几何…';
    const geometries = new Map();
    // Imports replace server resources even when their relative URLs are identical.
    // Keep the old cache alive until the replacement scene is ready to commit.
    if (fromImport || (state.data?.map && state.data.map.name !== data.map.name)
      || (state.data?.scene_id && state.data.scene_id !== data.scene_id)) {
      state.geometryCache = new Map();
      state.textureCache = new Map();
      cacheReplaced = true;
    }
    const world = await fetchGeometry(data.world.url, data.world.uv_url, data.world.groups, warnings, `地图 ${data.map.name}`);
    let nextModel = 0;
    let completed = 0;
    const modelRows = data.models || [];
    const loadModel = async () => {
      while (nextModel < modelRows.length) {
        const row = modelRows[nextModel++];
        try { geometries.set(row.id, await fetchGeometry(row.url, row.uv_url, row.groups, warnings, row.model)); }
        catch (error) { warnings.push(`模型载入失败：${row.model} (${error.message})`); console.warn('Model geometry failed:', row.model, error); }
        completed++;
        $('loading-text').textContent = `正在载入物件 ${completed} / ${modelRows.length}…`;
      }
    };
    await Promise.all(Array.from({ length: Math.min(6, modelRows.length) }, loadModel));
    const loadedTextures = await loadTextures(data, warnings);
    const previousSelection = fromImport ? -1 : state.selected;
    const previousSlice = Number($('slice').value);
    const firstLoad = fromImport || !state.data || !!state.data.empty || state.data.map?.name !== data.map.name;
    const oldRoot = state.root;
    state.data = data;
    state.root = new THREE.Group();
    state.root.name = 'agpw-scene';
    state.mapMaterials = [];
    state.materialCache = new Map();
    state.materialDefinitions = new Map((data.materials || []).map((material) => [material.id, material]));
    state.loadedTextures = loadedTextures;
    state.waypointMaterials = [];
    state.stats = { worldTriangles: data.world.triangles || 0, modelTypes: geometries.size,
      loadedTextures: loadedTextures.size, textureCount: (data.textures || []).length,
      materialCount: (data.materials || []).length };
    state.selection = null;
    state.selectionRadius = null;
    state.nodes = null;
    state.labels = [];
    $('labels').replaceChildren();
    const worldMesh = new THREE.Mesh(world, surfaceMaterials(world, '#a6b0b8', warnings));
    worldMesh.name = 'bsp-world';
    state.root.add(worldMesh);
    state.mapBounds = boxFromBounds(data.map.play_bounds) || boxFromBounds(data.map.bounds)
      || world.boundingBox?.clone() || new THREE.Box3(new THREE.Vector3(-500, -500, -100), new THREE.Vector3(500, 500, 500));
    buildProps(data, geometries, warnings);
    buildWaypoints(data);
    initializeEditor(data, options.saveHandleAfterImport || null);
    disposeTree(oldRoot);
    scene.add(state.root);
    sceneCommitted = true;
    if (cacheReplaced) {
      for (const geometry of oldCache.values()) geometry.dispose();
      for (const texture of oldTextureCache.values()) texture.dispose();
    }
    const sliceBounds = state.mapBounds.clone();
    if (state.waypointBounds) sliceBounds.union(state.waypointBounds);
    state.sliceMin = Math.floor(sliceBounds.min.z) - 10;
    state.sliceMax = Math.ceil(sliceBounds.max.z) + 10;
    $('slice').min = String(state.sliceMin);
    $('slice').max = String(state.sliceMax);
    $('slice').value = String(firstLoad ? state.sliceMax : THREE.MathUtils.clamp(previousSlice, state.sliceMin, state.sliceMax));
    $('slice-min').textContent = String(state.sliceMin);
    $('slice-max').textContent = String(state.sliceMax);
    $('locate-id').max = String(Math.max(0, state.points.length - 1));
    $('scene-name').textContent = `${data.map.name} · ${state.points.length} 个路点 · ${(data.props || []).length} 个地图物件`;
    document.title = `${data.map.name} · AgPW 3D 路点预览`;
    if (fromImport) state.imported = true;
    updateSourceNote(data);
    $('mode-badge').hidden = false;
    $('scene-stats').textContent = `${state.points.length} 个点 · ${state.stats.directedLinks} 条方向连接 · ${(data.world.triangles || 0).toLocaleString()} 个地图三角形`;
    showWarnings(warnings);
    showMapInfo(data);
    applyOpacity();
    applySlice();
    clearSelection();
    if (fromImport) $('locate-id').value = '';
    if (previousSelection >= 0 && previousSelection < state.points.length) selectWaypoint(previousSelection);
    if (firstLoad) fitWaypoints();
    state.ready = true;
    setStatus(`载入完成 · ${state.points.length} 个路点 · ${state.stats.loadedProps} 个地图物件${state.stats.missingProps ? ` · ${state.stats.missingProps} 个缺失物件已标记` : ''}`);
    requestRender();
    return true;
  } catch (error) {
    if (cacheReplaced && !sceneCommitted) {
      for (const geometry of state.geometryCache.values()) geometry.dispose();
      state.geometryCache = oldCache;
      for (const texture of state.textureCache.values()) texture.dispose();
      state.textureCache = oldTextureCache;
    }
    state.ready = wasReady;
    state.error = error.message;
    console.error('Viewer scene load failed:', error);
    if (fromImport) showImportFailure(error, options.filename || 'AGPW');
    else {
      $('fatal-text').textContent = `读取失败：${error.message}。请检查启动器终端，或重新导入路点文件。`;
      $('fatal').hidden = false;
      setStatus(`载入失败：${error.message}`);
    }
    return false;
  } finally {
    setBusy(false);
    $('loading').hidden = true;
  }
}
function pickWaypoint(event) {
  if (!state.nodes || !state.ready) return;
  const rect = renderer.domElement.getBoundingClientRect();
  pointer.set((event.clientX - rect.left) / rect.width * 2 - 1, -(event.clientY - rect.top) / rect.height * 2 + 1);
  raycaster.setFromCamera(pointer, camera);
  const hits = raycaster.intersectObject(state.nodes, false);
  for (const hit of hits) {
    const index = hit.instanceId;
    if (index === undefined || state.points[index].origin[2] > clipPlane.constant) continue;
    if (!$('xray').checked) {
      raycaster.far = Math.max(0, hit.distance - 1);
      const mapHits = raycaster.intersectObjects(state.root.children.filter((child) => child.name !== 'waypoints'), true);
      raycaster.far = Infinity;
      if (mapOccludes(mapHits)) continue;
    }
    selectWaypoint(index);
    return;
  }
}
// Keep markers readable across full-map views without moving their true centers.
// The limits are CSS pixels, independent of the display's device pixel ratio.
function updateMarkerSizes() {
  if (!state.nodes || !controls) return;
  const height = Math.max(1, renderer.domElement.clientHeight);
  const forward = camera.getWorldDirection(new THREE.Vector3());
  const position = new THREE.Vector3();
  const transform = new THREE.Matrix4();
  const scaleVector = new THREE.Vector3();
  const orthoUnits = (orthographic.top - orthographic.bottom) / (height * camera.zoom);
  const perspectiveFactor = 2 * Math.tan(THREE.MathUtils.degToRad(perspective.fov / 2)) / (height * camera.zoom);
  const unitsAt = (point) => camera.isOrthographicCamera ? orthoUnits
    : Math.max(camera.near, position.copy(point).sub(camera.position).dot(forward)) * perspectiveFactor;
  state.points.forEach((point, index) => {
    const center = new THREE.Vector3(...point.origin);
    const units = Math.max(.0001, unitsAt(center));
    const pixels = 6.5 / units;
    const scale = THREE.MathUtils.clamp(pixels, 3.75, 7.5) / pixels;
    state.markerScales[index] = scale;
    transform.makeScale(scale, scale, scale).setPosition(center);
    state.nodes.setMatrixAt(index, transform);
  });
  state.nodes.instanceMatrix.needsUpdate = true;
  state.nodes.computeBoundingSphere();
  for (const { mesh, indices } of state.markerOverlays) {
    indices.forEach((pointIndex, instanceIndex) => {
      const scale = state.markerScales[pointIndex];
      transform.makeScale(scale, scale, scale).setPosition(...state.points[pointIndex].origin);
      mesh.setMatrixAt(instanceIndex, transform);
    });
    mesh.instanceMatrix.needsUpdate = true;
    mesh.computeBoundingSphere();
  }
  if (state.selection && state.selected >= 0) state.selection.scale.setScalar(state.markerScales[state.selected]);
  for (const { mesh, arrows } of state.arrowSets) {
    arrows.forEach((arrow, index) => {
      const units = Math.max(.0001, unitsAt(arrow.center));
      // Short connections constrain arrow length/width, even when zoomed far out.
      const length = Math.min(THREE.MathUtils.clamp(arrow.length / units, 6.5, 11) * units, arrow.linkLength * .35);
      const radius = Math.min(THREE.MathUtils.clamp(3.8 / units, 1.75, 3.3) * units, arrow.linkLength * .13);
      scaleVector.set(radius / 3.8, length / 15, radius / 3.8);
      transform.compose(arrow.center, arrow.quaternion, scaleVector);
      mesh.setMatrixAt(index, transform);
    });
    mesh.instanceMatrix.needsUpdate = true;
    mesh.computeBoundingSphere();
  }
}
function animate(now) {
  requestAnimationFrame(animate);
  if (!renderer || !scene) return;
  const delta = previousFrameTime === null || !Number.isFinite(now)
    ? 0 : THREE.MathUtils.clamp((now - previousFrameTime) / 1000, 0, .05);
  previousFrameTime = Number.isFinite(now) ? now : null;
  updateFly(delta);
  if (state.renderNeeded) {
    if (!fly.enabled) controls?.update();
    updateMarkerSizes();
    updateCameraReadout();
    renderer.render(scene, camera);
    state.stats.drawCalls = renderer.info.render.calls;
    state.stats.renderedTriangles = renderer.info.render.triangles;
    state.renderNeeded = false;
  }
  if (state.labelNeeded && state.ready) updateLabels();
}
function installUI() {
  $('fit-waypoints').addEventListener('click', fitWaypoints);
  $('fit-map').addEventListener('click', () => fitBox(state.mapBounds));
  $('view-mode').addEventListener('click', () => setTopView(!state.topView));
  $('fly-mode').addEventListener('click', () => setFlyMode(!fly.enabled));
  $('fly-speed').addEventListener('input', () => {
    fly.speed = Number($('fly-speed').value);
    updateFlyUI();
  });
  $('reload').addEventListener('click', reloadWaypoints);
  $('save-waypoints').addEventListener('click', () => saveWaypoints(false));
  $('save-as').addEventListener('click', () => saveWaypoints(true));
  $('download-copy').addEventListener('click', () => saveWaypoints(false, true));
  $('undo-edit').addEventListener('click', () => { if (!state.loading && !state.saving) state.editor?.undo(); });
  $('redo-edit').addEventListener('click', () => { if (!state.loading && !state.saving) state.editor?.redo(); });
  window.addEventListener('beforeunload', (event) => {
    if (state.editor?.dirty) { event.preventDefault(); event.returnValue = ''; }
  });
  const chooseFile = () => {
    if (!state.loading && !state.saving) {
      suspendFly();
      $('import-file').click();
    }
  };
  $('import-agpw').addEventListener('click', chooseFile);
  $('empty-import').addEventListener('click', chooseFile);
  $('import-file').addEventListener('change', () => importWaypoint($('import-file').files[0]));
  $('slice').addEventListener('input', applySlice);
  $('slice-reset').addEventListener('click', () => { $('slice').value = String(state.sliceMax); applySlice(); });
  $('opacity').addEventListener('input', applyOpacity);
  $('show-textures').addEventListener('change', applyTextures);
  $('show-models').addEventListener('change', () => { if (state.models) state.models.visible = $('show-models').checked; requestRender(); });
  $('show-labels').addEventListener('change', requestRender);
  $('xray').addEventListener('change', applyXray);
  $('locate-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const raw = $('locate-id').value.trim();
    const index = Number(raw);
    if (!raw || !selectWaypoint(index, true)) $('locate-message').textContent = `请输入 0 到 ${Math.max(0, state.points.length - 1)} 之间的有效编号。`;
  });
  document.addEventListener('keydown', (event) => {
    const target = event.target;
    const nativeInput = target instanceof Element && target.closest('input, textarea, select, [contenteditable]');
    if (!nativeInput && event.ctrlKey && !event.altKey && !event.metaKey
      && ['z', 'y'].includes(event.key.toLowerCase())) {
      event.preventDefault();
      if (!state.loading && !state.saving) {
        if (event.key.toLowerCase() === 'y' || event.shiftKey) state.editor?.redo();
        else state.editor?.undo();
      }
      return;
    }
    if ((target instanceof Element && target.closest('input, textarea, select, button, [contenteditable]'))
      || event.ctrlKey || event.altKey || event.metaKey) return;
    if (fly.enabled && flyMovementActive() && !state.loading && FLY_KEYS.has(event.code)) {
      fly.keys.add(event.code);
      event.preventDefault();
      return;
    }
    switch (event.key.toLowerCase()) {
      case 'v': if (!event.repeat) setFlyMode(!fly.enabled); break;
      case 'f': fitWaypoints(); break;
      case 'g': fitBox(state.mapBounds); break;
      case 't': setTopView(!state.topView); break;
      case 'escape': if (fly.enabled) suspendFly(); else clearSelection(); break;
      case '[': case ']': {
        const step = event.key === '[' ? -32 : 32;
        $('slice').value = String(THREE.MathUtils.clamp(Number($('slice').value) + step, state.sliceMin, state.sliceMax));
        applySlice(); break;
      }
      default: return;
    }
    event.preventDefault();
  });
  document.addEventListener('keyup', (event) => { fly.keys.delete(event.code); });
  renderer.domElement.addEventListener('blur', suspendFly);
  window.addEventListener('blur', suspendFly);
  document.addEventListener('visibilitychange', () => { if (document.hidden) suspendFly(); });
  document.addEventListener('pointerlockchange', handleFlyPointerLockChange);
  document.addEventListener('pointerlockerror', () => {
    if (fly.inFlight) failFlyPointerLock(fly.inFlight, new Error('浏览器拒绝或无法完成鼠标接管。'));
  });
  document.addEventListener('mousemove', (event) => {
    if (fly.enabled && flyMovementActive() && fly.lockStatus === 'locked' && canvasPointerLocked()) {
      rotateFlyView(event.movementX || 0, event.movementY || 0);
    }
  });
  renderer.domElement.addEventListener('contextmenu', (event) => { if (fly.enabled) event.preventDefault(); });
  let down = null;
  let requestOnClick = false;
  renderer.domElement.addEventListener('pointerdown', (event) => {
    renderer.domElement.focus({ preventScroll: true });
    requestOnClick = false;
    if (event.button !== 0) { down = null; return; }
    down = { x: event.clientX, y: event.clientY, dragged: false, pointerId: event.pointerId, session: fly.session };
    if (fly.enabled && !canvasPointerLocked()) {
      fly.pointerId = event.pointerId;
      fly.lastX = event.clientX;
      fly.lastY = event.clientY;
      renderer.domElement.setPointerCapture(event.pointerId);
    }
  });
  renderer.domElement.addEventListener('pointermove', (event) => {
    // Locked look uses document mousemove only, so movement is never counted twice.
    if (!fly.enabled || canvasPointerLocked() || fly.pointerId !== event.pointerId || !down) return;
    if (down.session !== fly.session) { down = null; requestOnClick = false; return; }
    if (!(event.buttons & 1)) { down = null; resetFlyInput(); return; }
    if (Math.hypot(event.clientX - down.x, event.clientY - down.y) >= 5) down.dragged = true;
    if (down.dragged) {
      fly.engaged = true;
      if (fly.lockStatus === 'waiting') {
        fly.lockStatus = 'fallback';
        updateFlyUI();
      }
      renderer.domElement.classList.add('fly-looking');
      rotateFlyView(event.clientX - fly.lastX, event.clientY - fly.lastY);
      event.preventDefault();
    }
    fly.lastX = event.clientX;
    fly.lastY = event.clientY;
  });
  renderer.domElement.addEventListener('pointerup', (event) => {
    const clicked = event.button === 0 && down && down.session === fly.session && !down.dragged
      && Math.hypot(event.clientX - down.x, event.clientY - down.y) < 5;
    if (clicked) {
      if (fly.enabled && canvasPointerLocked()) {
        const rect = renderer.domElement.getBoundingClientRect();
        pickWaypoint({ clientX: rect.left + rect.width / 2, clientY: rect.top + rect.height / 2 });
      } else {
        // A short click can still inspect a waypoint; a fallback drag never picks.
        pickWaypoint(event);
        requestOnClick = fly.enabled;
      }
    }
    down = null;
    if (fly.pointerId === event.pointerId) {
      fly.pointerId = null;
      if (renderer.domElement.hasPointerCapture(event.pointerId)) renderer.domElement.releasePointerCapture(event.pointerId);
      renderer.domElement.classList.remove('fly-looking');
    }
  });
  renderer.domElement.addEventListener('click', (event) => {
    if (event.button === 0 && requestOnClick && fly.enabled && !canvasPointerLocked()) {
      requestOnClick = false;
      requestFlyPointerLock();
    }
  });
  renderer.domElement.addEventListener('pointercancel', () => { down = null; requestOnClick = false; resetFlyInput(); });
  renderer.domElement.addEventListener('lostpointercapture', (event) => {
    if (fly.pointerId === event.pointerId) {
      fly.pointerId = null;
      down = null;
      requestOnClick = false;
      renderer.domElement.classList.remove('fly-looking');
    }
  });
}
// Read-only inspection hooks for local smoke verification.
window.agpwViewer = {
  get scene() { return scene; }, get camera() { return camera; },
  get renderer() { return renderer; }, get controls() { return controls; },
  get stats() { return state.stats; }, get ready() { return state.ready; },
  get error() { return state.error; }, get data() { return state.data; },
  get editor() { return state.editor; }, get saving() { return state.saving; },
  get selected() { return state.selected; }, get topView() { return state.topView; },
  get flyState() {
    const direction = camera.getWorldDirection(new THREE.Vector3());
    return { enabled: fly.enabled, speed: fly.speed, position: camera.position.toArray(),
      yaw: THREE.MathUtils.radToDeg(Math.atan2(direction.y, direction.x)),
      pitch: -THREE.MathUtils.radToDeg(Math.asin(THREE.MathUtils.clamp(direction.z, -1, 1))),
      keys: [...fly.keys], focused: viewportFocused(), locked: canvasPointerLocked(),
      lockStatus: fly.lockStatus, lockError: fly.lockError, lockErrorDetail: fly.lockErrorDetail, requestPending: !!fly.inFlight, engaged: fly.engaged };
  },
  sourceAngleMatrix, selectWaypoint, clearSelection, loadScene, importWaypoint, setTopView, setFlyMode, requestFlyPointerLock, saveWaypoints, reloadWaypoints,
  fitWaypoints, fitMap: () => fitBox(state.mapBounds),
};
try {
  scene = new THREE.Scene();
  scene.add(new THREE.HemisphereLight('#e8f2ff', '#6c747e', 1.8));
  const light = new THREE.DirectionalLight('#ffffff', 2.0);
  light.position.set(400, -600, 1000);
  scene.add(light);
  const fill = new THREE.DirectionalLight('#a5bdd5', .55);
  fill.position.set(-300, 400, 200);
  scene.add(fill);
  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true, powerPreference: 'high-performance' });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.localClippingEnabled = true;
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.setClearColor('#19222d', 0);
  renderer.domElement.setAttribute('tabindex', '0');
  renderer.domElement.setAttribute('aria-label', '地图三维预览，左键旋转，Shift 加左键平移，滚轮缩放');
  $('canvas-host').append(renderer.domElement);
  perspective.position.set(1000, -1000, 850);
  makeControls();
  new ResizeObserver(resize).observe($('canvas-host'));
  resize();
  renderer.domElement.addEventListener('webglcontextlost', (event) => {
    event.preventDefault();
    state.ready = false;
    state.error = 'WebGL context lost';
    $('fatal-text').textContent = '浏览器的 3D 图形上下文已丢失，请刷新页面重新载入。';
    $('fatal').hidden = false;
  });
  installUI();
  animate();
  loadScene();
} catch (error) {
  state.error = error.message;
  console.error('Viewer initialization failed:', error);
  $('loading').hidden = true;
  $('fatal').hidden = false;
  $('fatal-text').textContent = '无法初始化 WebGL。请使用支持 WebGL 的浏览器并开启硬件加速，再通过启动器打开。详细信息：' + error.message;
  setStatus('3D 图形初始化失败。');
}



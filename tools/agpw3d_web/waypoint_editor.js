// Editing state is independent of the renderer and file transport.
// Add a field normalizer to the registry before exposing a future schema field.
export class FieldRegistry {
  constructor() { this.normalizers = new Map(); }
  register(key, normalize) { this.normalizers.set(key, normalize); return this; }
  has(key) { return this.normalizers.has(key); }
  normalize(field, value) { return this.normalizers.get(field.key)(value, field); }
}
function integer(value, field, fallbackMin, fallbackMax) {
  const number = Number(value);
  const min = Number.isFinite(field.min) ? field.min : fallbackMin;
  const max = Number.isFinite(field.max) ? field.max : fallbackMax;
  if (!['number', 'string'].includes(typeof value) || (typeof value === 'string' && !value.trim())
    || !Number.isInteger(number) || number < min || number > max) {
    throw new Error(`${field.label || field.key}必须是 ${min} 到 ${max} 之间的整数。`);
  }
  return number;
}
function float32(value, field) {
  const number = Number(value);
  const min = Number.isFinite(field.min) ? field.min : -2000;
  const max = Number.isFinite(field.max) ? field.max : 2000;
  const normalized = Math.fround(number);
  if (!['number', 'string'].includes(typeof value) || (typeof value === 'string' && !value.trim())
    || !Number.isFinite(number) || number < min || number > max
    || !Number.isFinite(normalized) || normalized < min || normalized > max) {
    throw new Error(`${field.label || field.key}必须是 ${min} 到 ${max} 之间的有限数值。`);
  }
  return normalized;
}
export function createFieldRegistry() {
  return new FieldRegistry()
    .register('radius', (value, field) => integer(value, field, 0, 255))
    .register('flags', (value, field) => integer(value, field, 0, 0xffffffff) >>> 0)
    .register('jumpVelocity', (value, field) => {
      if (!Array.isArray(value) || value.length !== 8
        || Array.from(value).some((vector) => !Array.isArray(vector) || vector.length !== 3)) {
        throw new Error('跳跃初速度必须包含 8 组 XYZ 分量。');
      }
      return value.map((vector) => Array.from(vector, (component) => float32(component, field)));
    });
}
const copyValue = (value) => Array.isArray(value) ? value.map(copyValue) : value;
const copyRow = (row) => Object.fromEntries(Object.entries(row).map(([key, value]) => [key, copyValue(value)]));
const copyRows = (rows) => rows.map(copyRow);
const sameValue = (left, right) => Array.isArray(left) && Array.isArray(right)
  ? left.length === right.length && left.every((value, index) => sameValue(value, right[index])) : left === right;
export class WaypointEditor {
  constructor(points, schema, onChange = () => {}, registry = createFieldRegistry()) {
    this.schema = schema;
    this.registry = registry;
    this.fields = (schema.fields || []).filter((field) => registry.has(field.key));
    this.flags = (schema.flags || []).map((flag) => ({ ...flag, bit: Number(flag.bit) >>> 0 }));
    this.knownFlagsMask = Number(schema.known_flags_mask) >>> 0;
    this.editableFlagsMask = this.flags.reduce((mask, flag) => (mask | flag.bit) >>> 0, 0);
    this.sourceHash = schema.source_hash;
    this.original = points.map((point) => Object.fromEntries(this.fields.map((field) => [field.key, registry.normalize(field, point[field.key])])));
    this.values = copyRows(this.original);
    this.saved = copyRows(this.original);
    this.commands = [];
    this.cursor = 0;
    this.revision = 0;
    this.onChange = onChange;
    this.jumpSlots = points.map((point, index) => (point.linkSlots || []).map(([target, flags]) =>
      Number.isInteger(target) && target >= 0 && target < points.length && target !== index && !!(flags & 1)));
  }
  get canUndo() { return this.cursor > 0; }
  get canRedo() { return this.cursor < this.commands.length; }
  get dirty() { return this.values.some((row, index) => this.fields.some((field) => !sameValue(row[field.key], this.saved[index][field.key]))); }
  getPoint(index) {
    if (!Number.isInteger(index) || index < 0 || index >= this.values.length) throw new Error('无效的路点编号。');
    return copyRow(this.values[index]);
  }
  unknownFlags(index) { return (this.getPoint(index).flags & ~this.knownFlagsMask) >>> 0; }
  mergeFlags(index, checkedMask) {
    return ((this.getPoint(index).flags & ~this.editableFlagsMask) | (checkedMask & this.editableFlagsMask)) >>> 0;
  }
  apply(index, updates) {
    const current = this.getPoint(index), before = {}, after = {};
    for (const field of this.fields) {
      if (!Object.hasOwn(updates, field.key)) continue;
      const value = this.registry.normalize(field, updates[field.key]);
      if (field.key === 'flags' && ((value & ~this.knownFlagsMask) >>> 0)
        !== ((this.original[index].flags & ~this.knownFlagsMask) >>> 0)) throw new Error('未知属性位必须保留，不能修改。');
      if (field.key === 'jumpVelocity' && value.some((vector, slot) =>
        !sameValue(vector, current.jumpVelocity[slot]) && !this.jumpSlots[index][slot])) {
        throw new Error('只能编辑已有有效 JUMP 出边的跳跃初速度。');
      }
      if (!sameValue(current[field.key], value)) { before[field.key] = current[field.key]; after[field.key] = value; }
    }
    if (!Object.keys(after).length) return false;
    this.commands.splice(this.cursor);
    this.commands.push({ index, before, after });
    this.cursor++;
    Object.assign(this.values[index], after);
    this.changed('apply', index);
    return true;
  }
  undo() {
    if (!this.canUndo) return false;
    const command = this.commands[--this.cursor];
    Object.assign(this.values[command.index], command.before);
    this.changed('undo', command.index);
    return true;
  }
  redo() {
    if (!this.canRedo) return false;
    const command = this.commands[this.cursor++];
    Object.assign(this.values[command.index], command.after);
    this.changed('redo', command.index);
    return true;
  }
  changed(kind, index) {
    this.revision++;
    this.onChange({ kind, index, revision: this.revision });
  }
  getChanges() {
    const changes = [];
    this.values.forEach((row, index) => {
      const change = { index };
      for (const field of this.fields) if (!sameValue(row[field.key], this.original[index][field.key])) change[field.key] = copyValue(row[field.key]);
      if (Object.keys(change).length > 1) changes.push(change);
    });
    return changes;
  }
  markSaved(revision) {
    if (revision !== this.revision) return false;
    this.saved = copyRows(this.values);
    this.onChange({ kind: 'saved', index: null, revision: this.revision });
    return true;
  }
}

#!/usr/bin/env python3
"""AgPB 路点图查看器（只读，图论视角，不按地图几何排布）。

读 .agpw 文件，把路点当节点、连线当边画出来：
  - 单向边画箭头，双向边不画箭头；
  - 边按 flag 上色：JUMP 红 / DOUBLE 蓝 / VISIBLE 绿 / 普通 灰；
    双向且两个方向的 flag 不同时画虚线；
  - 节点按路点 flag 上色（配色跟游戏内叠加层一致），点选后右侧列出
    出入边与每条边的 flag。

用法：
    py tools/agpw_view.py <file.agpw>
    py tools/agpw_view.py --stats [file.agpw] [--layout]
    py tools/agpw_view.py --smoke

不传文件时：优先找 AGENTS.md 里写的部署目标下的
addons/AgPB/waypoints/*.agpw，找不到就弹文件选择框。
本工具只以 "rb" 打开文件，不会写任何东西。
"""

import argparse
import math
import random
import re
import struct
import sys
from collections import defaultdict, deque
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

ROOT = Path(__file__).resolve().parent.parent

MAGIC = 0x50424741          # 'AGPB' 小端
VERSION = 1
HEADER_FMT = "<III32s32s"
RECORD_FMT = "<3fIBB8h8Hf"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
RECORD_SIZE = struct.calcsize(RECORD_FMT)

WP_FLAGS = [
    (1 << 1, "LIFT"), (1 << 2, "CROUCH"), (1 << 3, "CROSSING"), (1 << 4, "GOAL"),
    (1 << 5, "LADDER"), (1 << 6, "RESCUE"), (1 << 7, "CAMP"), (1 << 9, "DJUMP"),
    (1 << 11, "AVOID"), (1 << 12, "USEBUTTON"), (1 << 17, "FALLRISK"),
    (1 << 26, "FALLCHECK"), (1 << 27, "JUMP"), (1 << 28, "SNIPER"),
    (1 << 29, "T"), (1 << 30, "CT"),
]
PATH_FLAGS = [(1 << 0, "JUMP"), (1 << 1, "DOUBLE"), (1 << 2, "VISIBLE")]

# 配色 = wpdraw.cpp 的 NodeBasicColor / NodeFlagColor，逐值照抄（别自己调色）
NODE_BASIC_COLORS = [
    ("CAMP", "#00FFFF"), ("GOAL", "#8000FF"), ("LADDER", "#804000"), ("RESCUE", "#FFFFFF"),
    ("AVOID", "#FF0000"), ("FALLCHECK", "#808080"), ("USEBUTTON", "#0000FF"),
    ("FALLRISK", "#808080"), ("JUMP", "#FFFF00"), ("CROUCH", "#AA00FF"), ("LIFT", "#008080"),
]
NODE_DEFAULT_COLOR = "#00FF00"
NODE_OVERLAY_COLORS = [
    ("SNIPER", "#825700"), ("T", "#FF0000"), ("CT", "#0000FF"), ("FALLRISK", "#FA4B96"),
]
EDGE_COLORS = {"JUMP": "#e05252", "DOUBLE": "#4a7fd4", "VISIBLE": "#3fa34d", "": "#8a8a8a"}
EDGE_ORDER = ("JUMP", "DOUBLE", "VISIBLE", "")

NODE_R = 7.0
ARROW = 9.0


class Waypoint:
    __slots__ = ("origin", "flags", "radius", "mesh", "links", "gravity")

    def __init__(self, origin, flags, radius, mesh, index, conn_flags, gravity):
        self.origin = origin
        self.flags = flags
        self.radius = radius
        self.mesh = mesh
        self.gravity = gravity
        self.links = [(j, f) for j, f in zip(index, conn_flags) if j >= 0]


class WaypointFile:
    def __init__(self):
        self.path = None
        self.map_name = ""
        self.author = ""
        self.points = []
        self.pairs = {}

    def build_pairs(self):
        pairs = {}
        for i, node in enumerate(self.points):
            for j, flags in node.links:
                if j < 0 or j >= len(self.points) or j == i:
                    continue
                key = (i, j) if i < j else (j, i)
                entry = pairs.setdefault(key, {})
                entry[i] = entry.get(i, 0) | flags
        self.pairs = pairs

    def degrees(self):
        deg = defaultdict(int)
        for (a, b), dirs in self.pairs.items():
            if a in dirs:
                deg[a] += 1
            if b in dirs:
                deg[b] += 1
        return deg


def flag_names(value, table):
    return [name for bit, name in table if value & bit]


def pair_color(dirs):
    names = set()
    for flags in dirs.values():
        names.update(flag_names(flags, PATH_FLAGS))
    for key in EDGE_ORDER[:-1]:
        if key in names:
            return key
    return ""


def load_agpw(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError("file too small (%d bytes)" % len(data))

    magic, version, count, map_name, author = struct.unpack_from(HEADER_FMT, data, 0)
    if magic != MAGIC:
        raise ValueError("bad magic 0x%08X (not an .agpw)" % magic)
    if version != VERSION:
        raise ValueError("unsupported version %d (expected %d)" % (version, VERSION))
    if count > 8192:
        raise ValueError("bad waypoint count %d" % count)
    need = HEADER_SIZE + count * RECORD_SIZE
    if len(data) < need:
        raise ValueError("truncated: need %d bytes, have %d" % (need, len(data)))

    wf = WaypointFile()
    wf.path = str(path)
    wf.map_name = map_name.split(b"\0", 1)[0].decode("utf-8", "replace")
    wf.author = author.split(b"\0", 1)[0].decode("utf-8", "replace")

    off = HEADER_SIZE
    for _ in range(count):
        (x, y, z, flags, radius, mesh, i0, i1, i2, i3, i4, i5, i6, i7,
         c0, c1, c2, c3, c4, c5, c6, c7, gravity) = struct.unpack_from(RECORD_FMT, data, off)
        off += RECORD_SIZE
        wf.points.append(Waypoint((x, y, z), flags, radius, mesh,
                                  (i0, i1, i2, i3, i4, i5, i6, i7),
                                  (c0, c1, c2, c3, c4, c5, c6, c7), gravity))

    wf.build_pairs()
    return wf


# ---------------------------------------------------------------------------
# 图论布局：按连通分量分开摆，每个分量跑一遍力导向
# ---------------------------------------------------------------------------

def connected_components(n, pair_keys):
    adj = defaultdict(list)
    for a, b in pair_keys:
        adj[a].append(b)
        adj[b].append(a)
    seen = [False] * n
    comps = []
    for start in range(n):
        if seen[start]:
            continue
        comp = []
        queue = deque([start])
        seen[start] = True
        while queue:
            cur = queue.popleft()
            comp.append(cur)
            for nxt in adj[cur]:
                if not seen[nxt]:
                    seen[nxt] = True
                    queue.append(nxt)
        comps.append(comp)
    return comps


def force_layout(nodes, pairs, area=1.0, iters=220, seed=12345):
    """Fruchterman-Reingold；斥力用网格截断，几千个点也不至于卡死。"""
    rnd = random.Random(seed)
    n = len(nodes)
    if n == 1:
        return {nodes[0]: (0.5, 0.5)}
    if n == 2:
        return {nodes[0]: (0.25, 0.5), nodes[1]: (0.75, 0.5)}

    k = math.sqrt(area / n)
    pos = {i: (rnd.random(), rnd.random()) for i in nodes}
    temp = 0.12 * math.sqrt(area)
    cutoff = 3.0 * k
    cell = max(cutoff, 1e-6)

    adj = defaultdict(list)
    for a, b in pairs:
        adj[a].append(b)
        adj[b].append(a)

    for _ in range(iters):
        grid = defaultdict(list)
        for i in nodes:
            grid[(int(pos[i][0] / cell), int(pos[i][1] / cell))].append(i)

        disp = {i: [0.0, 0.0] for i in nodes}

        for i in nodes:
            xi, yi = pos[i]
            gx, gy = int(xi / cell), int(yi / cell)
            for ox in (-1, 0, 1):
                for oy in (-1, 0, 1):
                    for j in grid.get((gx + ox, gy + oy), ()):
                        if j <= i:
                            continue
                        dx, dy = xi - pos[j][0], yi - pos[j][1]
                        d2 = dx * dx + dy * dy
                        if d2 < 1e-9:
                            dx, dy, d2 = 1e-3, 1e-3, 2e-6
                        if d2 > cutoff * cutoff:
                            continue
                        d = math.sqrt(d2)
                        f = (k * k) / d
                        ux, uy = dx / d, dy / d
                        disp[i][0] += ux * f
                        disp[i][1] += uy * f
                        disp[j][0] -= ux * f
                        disp[j][1] -= uy * f

        for a, b in pairs:
            dx, dy = pos[a][0] - pos[b][0], pos[a][1] - pos[b][1]
            d = math.sqrt(dx * dx + dy * dy) or 1e-3
            f = (d * d) / k
            ux, uy = dx / d, dy / d
            disp[a][0] -= ux * f
            disp[a][1] -= uy * f
            disp[b][0] += ux * f
            disp[b][1] += uy * f

        for i in nodes:
            dx, dy = disp[i]
            d = math.sqrt(dx * dx + dy * dy)
            if d > 1e-9:
                step = min(d, temp)
                pos[i] = (min(0.98, max(0.02, pos[i][0] + dx / d * step)),
                          min(0.98, max(0.02, pos[i][1] + dy / d * step)))
        temp = max(temp * 0.985, 0.0005)

    return pos


def graph_layout(wf, iters=None):
    n = len(wf.points)
    if n == 0:
        return {}
    comps = connected_components(n, wf.pairs.keys())
    comps.sort(key=len, reverse=True)
    if iters is None:
        iters = 260 if n <= 300 else (150 if n <= 900 else 60)

    results = []
    for comp in comps:
        if len(comp) == 1:
            results.append((comp, {comp[0]: (0.5, 0.5)}))
            continue
        comp_set = set(comp)
        local_pairs = [p for p in wf.pairs.keys() if p[0] in comp_set and p[1] in comp_set]
        results.append((comp, force_layout(comp, local_pairs, iters=iters)))

    total = sum(math.sqrt(len(c)) for c, _ in results) or 1.0
    unit = 900.0 / total
    cols = max(1, int(math.ceil(math.sqrt(len(results)))))
    col_w = 900.0 / cols

    positions = {}
    row_y = 0.0
    row_h = 0.0
    for idx, (comp, pos) in enumerate(results):
        col = idx % cols
        if col == 0 and idx > 0:
            row_y += row_h + 60.0
            row_h = 0.0
        size = math.sqrt(len(comp)) * unit
        row_h = max(row_h, size)
        for i, (px, py) in pos.items():
            positions[i] = (col * col_w + px * size, row_y + py * size)
    return positions


def default_waypoint_dir():
    """AGENTS.md 里写的部署目标 → addons/AgPB/waypoints。"""
    agents = ROOT / "AGENTS.md"
    if agents.is_file():
        match = re.search(r"部署目标[：:]\s*`([^`]+)`", agents.read_text(encoding="utf-8"))
        if match:
            wp_dir = Path(match.group(1).strip()) / "addons" / "AgPB" / "waypoints"
            if wp_dir.is_dir():
                return wp_dir
    local = ROOT / "build" / "package" / "addons" / "AgPB" / "waypoints"
    if local.is_dir():
        return local
    return None


class Viewer(tk.Tk):
    def __init__(self, path=None):
        super().__init__()
        self.title("AgPB 路点图查看器（只读）")
        self.geometry("1380x860")
        self.wf = None
        self.positions = {}
        self.zoom = 1.0
        self.pan = [0.0, 0.0]
        self.selected = None
        self.show_labels = tk.BooleanVar(value=True)
        self.show_isolated = tk.BooleanVar(value=True)
        self.edge_filters = {name: tk.BooleanVar(value=True) for name in EDGE_ORDER}
        self._drag_node = None
        self._drag_last = None

        self._build_ui()

        if path:
            self.load(path)
        else:
            guess = default_waypoint_dir()
            if guess:
                files = sorted(guess.glob("*.agpw"))
                if len(files) == 1:
                    self.load(files[0])
                elif files:
                    self.status.configure(text="发现 %d 个 .agpw，用「打开...」选一个：%s"
                                               % (len(files), ", ".join(f.name for f in files[:6])))

    def _build_ui(self):
        bar = ttk.Frame(self, padding=(6, 4))
        bar.pack(side=tk.TOP, fill=tk.X)

        ttk.Button(bar, text="打开...", command=self.on_open).pack(side=tk.LEFT)
        ttk.Button(bar, text="重载", command=self.on_reload).pack(side=tk.LEFT, padx=(4, 10))
        ttk.Button(bar, text="重新布局", command=self.relayout).pack(side=tk.LEFT)
        ttk.Button(bar, text="适应窗口", command=self.fit).pack(side=tk.LEFT, padx=(4, 10))
        ttk.Checkbutton(bar, text="编号", variable=self.show_labels,
                        command=self.draw).pack(side=tk.LEFT)
        ttk.Checkbutton(bar, text="孤立点", variable=self.show_isolated,
                        command=self.draw).pack(side=tk.LEFT, padx=(4, 10))
        ttk.Label(bar, text="边:").pack(side=tk.LEFT)
        for name in EDGE_ORDER:
            ttk.Checkbutton(bar, text=name or "普通", variable=self.edge_filters[name],
                            command=self.draw).pack(side=tk.LEFT, padx=1)

        body = ttk.Frame(self)
        body.pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        self.canvas = tk.Canvas(body, background="#1e1f22", highlightthickness=0)
        self.canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        side = ttk.Frame(body, width=400, padding=(6, 4))
        side.pack(side=tk.RIGHT, fill=tk.Y)
        side.pack_propagate(False)
        self.info = tk.Text(side, width=48, wrap=tk.NONE, font=("Consolas", 10))
        self.info.pack(side=tk.TOP, fill=tk.BOTH, expand=True)
        self.info.configure(state=tk.DISABLED)
        legend = tk.Text(side, width=48, height=11, wrap=tk.NONE, font=("Consolas", 9))
        legend.pack(side=tk.BOTTOM, fill=tk.X)
        legend.insert("1.0",
                      "图例\n"
                      " 节点色 = 路点 flag（与游戏内一致）：CAMP青 GOAL紫 LADDER棕 RESCUE白\n"
                      "          AVOID红 FALLCHECK灰 USEBUTTON蓝 JUMP黄 CROUCH紫罗兰 LIFT墨绿\n"
                      "          其它绿；描边 T红 / CT蓝 / SNIPER暗金 / FALLRISK粉\n"
                      " 边：无箭头=两向 flag 相同的双向边；带箭头=单向，或两向不同（每向一条）\n"
                      "     颜色 JUMP红 DOUBLE蓝 VISIBLE绿 普通灰\n"
                      " 操作：滚轮缩放 | 空白拖动平移 | 拖节点微调 | 点节点看明细")
        legend.configure(state=tk.DISABLED)

        self.status = ttk.Label(self, anchor=tk.W, padding=(6, 2))
        self.status.pack(side=tk.BOTTOM, fill=tk.X)

        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_motion)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<MouseWheel>", self.on_wheel)
        self.bind("<F5>", lambda _e: self.relayout())
        self.bind("<Control-o>", lambda _e: self.on_open())

    def on_open(self):
        guess = default_waypoint_dir()
        path = filedialog.askopenfilename(
            title="选择 .agpw 路点文件",
            initialdir=str(guess) if guess else str(ROOT),
            filetypes=[("AgPB waypoints", "*.agpw"), ("All files", "*.*")])
        if path:
            self.load(path)

    def load(self, path):
        try:
            self.wf = load_agpw(path)
        except (OSError, ValueError) as exc:
            messagebox.showerror("读取失败", str(exc))
            return
        self.selected = None
        self.info.configure(state=tk.NORMAL)
        self.info.delete("1.0", tk.END)
        self.info.insert("1.0", "已载入 %s\n点任意节点看明细。" % path)
        self.info.configure(state=tk.DISABLED)
        self.relayout()

    def on_reload(self):
        if self.wf and self.wf.path:
            self.load(self.wf.path)

    def relayout(self):
        if not self.wf:
            return
        self.positions = graph_layout(self.wf)
        self.fit()

    def fit(self):
        if not self.positions:
            return
        xs = [p[0] for p in self.positions.values()]
        ys = [p[1] for p in self.positions.values()]
        w = max(1.0, max(xs) - min(xs))
        h = max(1.0, max(ys) - min(ys))
        cw = max(1, self.canvas.winfo_width())
        ch = max(1, self.canvas.winfo_height())
        self.zoom = max(0.05, min(cw / (w + 120.0), ch / (h + 120.0)))
        self.pan[0] = (cw - w * self.zoom) * 0.5 - min(xs) * self.zoom
        self.pan[1] = (ch - h * self.zoom) * 0.5 - min(ys) * self.zoom
        self.draw()

    def to_screen(self, x, y):
        return x * self.zoom + self.pan[0], y * self.zoom + self.pan[1]

    def to_world(self, sx, sy):
        return (sx - self.pan[0]) / self.zoom, (sy - self.pan[1]) / self.zoom

    def visible_pairs(self):
        out = []
        for (a, b), dirs in self.wf.pairs.items():
            if any(self.edge_filters[pair_color({d: f})].get() for d, f in dirs.items()):
                out.append((a, b, dirs))
        return out

    def draw(self):
        if not self.wf:
            return
        c = self.canvas
        c.delete("all")
        deg = self.wf.degrees()
        visible = self.visible_pairs()

        for a, b, dirs in visible:
            if not self.show_isolated.get() and (deg.get(a, 0) == 0 or deg.get(b, 0) == 0):
                continue
            x1, y1 = self.to_screen(*self.positions[a])
            x2, y2 = self.to_screen(*self.positions[b])
            highlighted = self.selected in (a, b)

            # 两向 flag 完全相同 = 真正的双向边，画一条无箭头的线；
            # 其余情况（单向 / 两向 flag 不同，比如 JUMP 跳过去 + 普通走回来）
            # 每个方向各画一条带箭头的线，方向信息不吃掉。
            if len(dirs) == 2 and dirs[a] == dirs[b]:
                color = "#ff9f40" if highlighted else EDGE_COLORS[pair_color(dirs)]
                c.create_line(x1, y1, x2, y2, fill=color, width=2, tags=("edge",))
                if highlighted:
                    names = "+".join(flag_names(dirs[a], PATH_FLAGS)) or "plain"
                    c.create_text((x1 + x2) / 2, (y1 + y2) / 2 - 4, text=names,
                                  fill="#ffd08a", font=("Consolas", 8))
                continue

            dx, dy = x2 - x1, y2 - y1
            length = math.hypot(dx, dy) or 1.0
            ox, oy = -dy / length * 3.0, dx / length * 3.0
            for src, flags in sorted(dirs.items()):
                if not self.edge_filters[pair_color({src: flags})].get():
                    continue
                dst = b if src == a else a
                sx, sy = self.to_screen(*self.positions[src])
                ex, ey = self.to_screen(*self.positions[dst])
                sign = 1.0 if src == a else -1.0
                sx, sy = sx + ox * sign, sy + oy * sign
                ex, ey = ex + ox * sign, ey + oy * sign
                color = "#ff9f40" if highlighted else EDGE_COLORS[pair_color({src: flags})]
                c.create_line(sx, sy, ex, ey, fill=color, width=2, tags=("edge",))
                self._arrow(sx, sy, ex, ey, color)
                if highlighted:
                    names = "+".join(flag_names(flags, PATH_FLAGS)) or "plain"
                    c.create_text((sx + ex) / 2, (sy + ey) / 2 - 4, text=names,
                                  fill="#ffd08a", font=("Consolas", 8))

        for i, node in enumerate(self.wf.points):
            if not self.show_isolated.get() and deg.get(i, 0) == 0:
                continue
            sx, sy = self.to_screen(*self.positions[i])
            r = NODE_R
            names = flag_names(node.flags, WP_FLAGS)
            color = NODE_DEFAULT_COLOR
            for key, value in NODE_BASIC_COLORS:
                if key in names:
                    color = value
                    break
            outline = "#FFFFFF"
            for key, value in NODE_OVERLAY_COLORS:
                if key in names:
                    outline = value
                    break
            if i == self.selected:
                outline, r = "#ff9f40", r + 2
            c.create_oval(sx - r, sy - r, sx + r, sy + r, fill=color,
                          outline=outline, width=2, tags=("node", i))
            if self.show_labels.get() and (self.zoom > 0.5 or len(self.wf.points) <= 120):
                c.create_text(sx + r + 2, sy - r, text=str(i), anchor=tk.NW,
                              fill="#cfcfcf", font=("Consolas", 8))

        self._update_status(len(visible))

    def _arrow(self, x1, y1, x2, y2, color):
        dx, dy = x2 - x1, y2 - y1
        length = math.hypot(dx, dy)
        if length < 1e-6:
            return
        ux, uy = dx / length, dy / length
        tipx, tipy = x2 - ux * NODE_R, y2 - uy * NODE_R
        basex, basey = tipx - ux * ARROW, tipy - uy * ARROW
        px, py = -uy, ux
        self.canvas.create_polygon(
            tipx, tipy,
            basex + px * ARROW * 0.42, basey + py * ARROW * 0.42,
            basex - px * ARROW * 0.42, basey - py * ARROW * 0.42,
            fill=color, outline=color, tags=("edge",))

    def _update_status(self, edge_count):
        wf = self.wf
        one_way = asym = sym = 0
        for (a, b), dirs in wf.pairs.items():
            if len(dirs) == 1:
                one_way += 1
            elif dirs[a] == dirs[b]:
                sym += 1
            else:
                asym += 1
        self.status.configure(
            text="%s ｜ map=%s ｜ author=%s ｜ %d 点 ｜ 显示 %d 条边"
                 "（单向 %d / 双向同 flag %d / 两向不同 %d）"
                 % (Path(wf.path).name if wf.path else "?", wf.map_name or "?", wf.author or "?",
                    len(wf.points), edge_count, one_way, sym, asym))

    def node_at(self, sx, sy):
        if not self.wf:
            return None
        best, best_d = None, NODE_R * self.zoom + 6
        for i, pos in self.positions.items():
            px, py = self.to_screen(*pos)
            d = math.hypot(px - sx, py - sy)
            if d <= best_d:
                best, best_d = i, d
        return best

    def on_press(self, event):
        node = self.node_at(event.x, event.y)
        self._drag_node = node
        self._drag_last = (event.x, event.y)
        if node is not None:
            self.selected = node
            self.show_info(node)
            self.draw()

    def on_motion(self, event):
        if self._drag_last is None:
            return
        dx = event.x - self._drag_last[0]
        dy = event.y - self._drag_last[1]
        if self._drag_node is None:
            self.pan[0] += dx
            self.pan[1] += dy
        else:
            wx, wy = self.positions[self._drag_node]
            self.positions[self._drag_node] = (wx + dx / self.zoom, wy + dy / self.zoom)
        self._drag_last = (event.x, event.y)
        self.draw()

    def on_release(self, _event):
        self._drag_node = None
        self._drag_last = None

    def on_wheel(self, event):
        wx, wy = self.to_world(event.x, event.y)
        self.zoom *= 1.1 if event.delta > 0 else 1 / 1.1
        self.pan[0] = event.x - wx * self.zoom
        self.pan[1] = event.y - wy * self.zoom
        self.draw()

    def show_info(self, idx):
        wf = self.wf
        node = wf.points[idx]
        deg = wf.degrees()
        lines = [
            "路点 #%d" % idx,
            "  origin  : %.1f %.1f %.1f" % node.origin,
            "  flags   : %s" % (", ".join(flag_names(node.flags, WP_FLAGS)) or "none"),
            "  radius  : %d" % node.radius,
            "  mesh    : %d" % node.mesh,
            "  gravity : %.3f" % node.gravity,
            "  出度    : %d" % deg.get(idx, 0),
            "",
            "出边（本点 -> 目标）:",
        ]
        for dst, flags in node.links:
            names = "+".join(flag_names(flags, PATH_FLAGS)) or "-"
            back = dict(wf.points[dst].links).get(idx) if 0 <= dst < len(wf.points) else None
            lines.append("  -> #%-4d [%-14s] %s" % (dst, names, "双向" if back is not None else "单向"))
        lines.append("")
        lines.append("入边（来源 -> 本点）:")
        for src, src_node in enumerate(wf.points):
            for dst, flags in src_node.links:
                if dst == idx:
                    names = "+".join(flag_names(flags, PATH_FLAGS)) or "-"
                    lines.append("  <- #%-4d [%s]" % (src, names))
        self.info.configure(state=tk.NORMAL)
        self.info.delete("1.0", tk.END)
        self.info.insert("1.0", "\n".join(lines))
        self.info.configure(state=tk.DISABLED)


def print_stats(path, run_layout=False):
    wf = load_agpw(path)
    one_way = sum(1 for dirs in wf.pairs.values() if len(dirs) == 1)
    two_way = len(wf.pairs) - one_way
    flag_count, edge_flag_count = defaultdict(int), defaultdict(int)
    for node in wf.points:
        for name in flag_names(node.flags, WP_FLAGS):
            flag_count[name] += 1
    for dirs in wf.pairs.values():
        for flags in dirs.values():
            for name in flag_names(flags, PATH_FLAGS):
                edge_flag_count[name] += 1
    print("file    : %s" % path)
    print("map     : %s  author: %s" % (wf.map_name, wf.author))
    print("points  : %d" % len(wf.points))
    print("edges   : %d (one-way %d / two-way %d)" % (len(wf.pairs), one_way, two_way))
    print("wp flags: %s" % (", ".join("%s=%d" % kv for kv in sorted(flag_count.items())) or "none"))
    print("ed flags: %s" % (", ".join("%s=%d" % kv for kv in sorted(edge_flag_count.items())) or "none"))
    if run_layout:
        pos = graph_layout(wf)
        xs = [p[0] for p in pos.values()]
        ys = [p[1] for p in pos.values()]
        print("layout  : %d nodes, bbox %.0f x %.0f"
              % (len(pos), max(xs) - min(xs), max(ys) - min(ys)))


def main():
    parser = argparse.ArgumentParser(description="AgPB .agpw waypoint graph viewer (read-only)")
    parser.add_argument("file", nargs="?", help="path to a .agpw file")
    parser.add_argument("--stats", action="store_true", help="print stats and exit (no GUI)")
    parser.add_argument("--layout", action="store_true", help="also run the layout in --stats mode")
    parser.add_argument("--smoke", action="store_true", help="create the window and close it (self-test)")
    args = parser.parse_args()

    path = args.file
    if not path:
        guess = default_waypoint_dir()
        if guess:
            files = sorted(guess.glob("*.agpw"))
            if len(files) == 1:
                path = files[0]

    if args.stats:
        if not path:
            print("no .agpw file given and none found; pass a path explicitly")
            return 1
        print_stats(path, run_layout=args.layout)
        return 0

    app = Viewer(path)
    if args.smoke:
        app.update()
        app.destroy()
        print("smoke ok")
        return 0
    app.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

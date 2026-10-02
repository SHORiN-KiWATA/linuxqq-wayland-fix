/*
 * LinuxQQ Share Overlay Fix —— 纯 Wayland 下把 QQ 共享悬浮层钉到固定位置。
 *
 * 背景：QQ 共享时创建三个普通 xdg_toplevel（class=QQ，标题「屏幕共享」）：
 *   - 内容/边框层：比屏幕大（QQ 拿物理分辨率当逻辑尺寸），应铺满屏幕、放在 0,0
 *   - 悬浮工具栏：应出现在共享区域顶部居中
 *   - 参会者面板：应出现在右上角
 * Wayland 协议不允许客户端给顶层窗口定位，QQ 的定位意图丢失，合成器只能层叠摆放。
 * 这个脚本在合成器侧（KWin）替 QQ 摆正。只改窗口位置/尺寸，不碰 QQ 进程。
 *
 * 实测：QQ 的位置意图本来就是逻辑（DIP）坐标，且跨缩放恒定——工具栏恒为「逻辑居中、
 * 距顶 24」，所以这里直接用逻辑屏计算即可，天然适配 100/125/150/175/200% 各档缩放。
 *
 * 安装见 ../../README.md。
 */

// 顶部居中工具栏距屏幕上边缘（逻辑像素）
var TOOLBAR_TOP = 24;
// 右上角面板距右、上边缘（逻辑像素）
var PANEL_RIGHT = 50;
var PANEL_TOP = 90;
// 单个窗口最多尝试摆几次，避免客户端拒绝尺寸时和信号来回打架
var MAX_ATTEMPTS = 6;

// KWin 6 的 JS 引擎没有注入 Qt，frameGeometry 不能构造 QRectF；
// 直接赋一个普通对象（异步生效）即可，KWin 会转成 QRectF。
function screenRect() {
    var s = workspace.activeScreen;
    if (s && s.geometry)
        return s.geometry;
    if (workspace.screens && workspace.screens.length > 0)
        return workspace.screens[0].geometry;
    return { x: 0, y: 0, width: 1920, height: 1080 };
}

function isOverlay(w) {
    return w && !w.deleted &&
        String(w.resourceClass) === "QQ" &&
        String(w.caption).indexOf("屏幕共享") !== -1;
}

function targetFor(g, s) {
    if (g.width > s.width || g.height > s.height) {
        // 内容/边框层（QQ 拿物理分辨率当逻辑尺寸，比屏幕大）：铺满当前屏幕
        return { x: s.x, y: s.y, width: s.width, height: s.height };
    }
    if (g.width > g.height * 2) {
        // 工具栏：顶部居中（实测工具栏宽高比远大于 2，面板接近 1）
        return { x: s.x + Math.round((s.width - g.width) / 2),
                 y: s.y + TOOLBAR_TOP, width: g.width, height: g.height };
    }
    // 参会者面板：右上角
    return { x: s.x + s.width - g.width - PANEL_RIGHT,
             y: s.y + PANEL_TOP, width: g.width, height: g.height };
}

var attempts = {};
var hooked = {};

function place(w) {
    if (!isOverlay(w))
        return;
    var id = w.internalId;
    var g = w.frameGeometry;
    var t = targetFor(g, screenRect());
    if (g.x === t.x && g.y === t.y && g.width === t.width && g.height === t.height)
        return;
    var n = attempts[id] || 0;
    if (n >= MAX_ATTEMPTS)
        return;
    attempts[id] = n + 1;
    try {
        w.frameGeometry = t;
    } catch (e) {
        print("qq-overlay: set geometry failed: " + e);
    }
}

function watch(w) {
    if (!isOverlay(w))
        return;
    var id = w.internalId;
    if (!hooked[id]) {
        hooked[id] = true;
        w.frameGeometryChanged.connect(function () { place(w); });
        w.closed.connect(function () { delete hooked[id]; delete attempts[id]; });
    }
    place(w);
}

function scan() {
    var l = workspace.stackingOrder;
    for (var i = 0; i < l.length; i++)
        watch(l[i]);
}

workspace.windowAdded.connect(function (w) {
    watch(w);
    place(w);
});

scan();
print("linuxqq-share-overlay: loaded");

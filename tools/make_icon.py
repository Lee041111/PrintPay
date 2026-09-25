# -*- coding: utf-8 -*-
"""生成程序图标：蓝色圆角底 + 白色打印机 + 绿点，输出 PNG 与多尺寸 ICO（纯标准库实现）。

做法：按目标尺寸的 4 倍画图（超采样），再降采样平均，得到平滑边缘。
"""
import struct
import zlib
import os

ASSETS = "D:/AiCode/PrintPay/assets"
SS = 4  # 超采样倍数


def write_png(path, width, height, pixels):
    """pixels: 字节数组，RGBA 顺序，长度 width*height*4"""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # 过滤器类型 0
        raw.extend(pixels[y * stride:(y + 1) * stride])

    def chunk(tag, data):
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def draw(size):
    """按 4 倍尺寸绘制，返回降采样后的 RGBA 像素（字节数组）"""
    n = size * SS
    # 画布（RGBA）
    buf = bytearray(n * n * 4)

    def put(x, y, rgba):
        if 0 <= x < n and 0 <= y < n:
            i = (y * n + x) * 4
            buf[i:i + 4] = bytes(rgba)

    def in_round_rect(x, y, left, top, right, bottom, radius):
        """圆角矩形判定：把点夹到“内矩形”上得到圆心，再比距离（标准做法，避免角落漏判）"""
        if x < left or x >= right or y < top or y >= bottom:
            return False
        in_left = x < left + radius
        in_right = x >= right - radius
        in_top = y < top + radius
        in_bottom = y >= bottom - radius
        if (in_left or in_right) and (in_top or in_bottom):
            cx = left + radius if in_left else right - 1 - radius
            cy = top + radius if in_top else bottom - 1 - radius
            return (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius
        return True

    def fill_round_rect(left, top, right, bottom, radius, color_from, color_to=None):
        for y in range(top, bottom):
            for x in range(left, right):
                if in_round_rect(x, y, left, top, right, bottom, radius):
                    if color_to is None:
                        # 统一补上 alpha 通道：调用方传 3 元组（RGB）时补成 RGBA，
                        # 否则写进 RGBA 画布会少一个字节（会把画布越写越短）
                        color = tuple(color_from) if len(color_from) == 4 else tuple(color_from) + (255,)
                        put(x, y, color)
                    else:
                        # 45 度方向的线性渐变
                        t = ((x - left) + (y - top)) / float((right - left) + (bottom - top))
                        color = tuple(int(color_from[k] + (color_to[k] - color_from[k]) * t)
                                      for k in range(3)) + (255,)
                        put(x, y, color)

    def u(v):  # 百分比 -> 像素
        return int(round(v * n / 100.0))

    # 背景：蓝色圆角方块
    fill_round_rect(0, 0, n, n, u(21), (26, 79, 138), (43, 127, 212))
    # 打印机：进纸 / 机身 / 出纸（白色）
    fill_round_rect(u(30), u(15), u(70), u(32), u(1), (255, 255, 255))
    fill_round_rect(u(15), u(35), u(85), u(66), u(2), (255, 255, 255))
    fill_round_rect(u(30), u(65), u(70), u(86), u(1), (255, 255, 255))
    # 机身上的绿色工作指示点
    for y in range(u(41), u(50)):
        for x in range(u(69), u(78)):
            if (x - u(73.5)) ** 2 + (y - u(45.5)) ** 2 <= (u(4.5)) ** 2:
                put(x, y, (17, 122, 55, 255))

    # 降采样：4x4 平均
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            r = g = b = a = 0
            for dy in range(SS):
                for dx in range(SS):
                    i = ((y * SS + dy) * n + (x * SS + dx)) * 4
                    if i + 3 >= len(buf):
                        raise IndexError("size=%d n=%d y=%d x=%d dy=%d dx=%d i=%d len=%d"
                                         % (size, n, y, x, dy, dx, i, len(buf)))
                    r += buf[i]
                    g += buf[i + 1]
                    b += buf[i + 2]
                    a += buf[i + 3]
            k = SS * SS
            o = (y * size + x) * 4
            out[o] = r // k
            out[o + 1] = g // k
            out[o + 2] = b // k
            out[o + 3] = a // k
    return out


def png_bytes(size):
    path = os.path.join(ASSETS, "icon_%d.png" % size)
    write_png(path, size, size, draw(size))
    with open(path, "rb") as handle:
        return handle.read()


def build_ico(sizes, out_path):
    """把多张 PNG 打包成 ICO（Vista+ 支持 PNG 格式的图标项）"""
    images = [(s, png_bytes(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(images))   # reserved, type=1(icon), count
    offset = 6 + 16 * len(images)

    entries = b""
    data = b""
    for size, blob in images:
        w = 0 if size >= 256 else size        # 256 用 0 表示
        h = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(blob), offset)
        data += blob
        offset += len(blob)

    with open(out_path, "wb") as handle:
        handle.write(header + entries + data)


if __name__ == "__main__":
    os.makedirs(ASSETS, exist_ok=True)
    build_ico([16, 32, 48, 256], os.path.join(ASSETS, "app.ico"))
    print("已生成: assets/app.ico 以及 icon_16/32/256.png")
    print("app.ico 大小: %d 字节" % os.path.getsize(os.path.join(ASSETS, "app.ico")))

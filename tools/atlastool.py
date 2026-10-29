#!/usr/bin/env python3
import argparse
import json
import math
from pathlib import Path
from PIL import Image
import numpy as np


def hex_to_rgb(hex_str: str) -> tuple[int, int, int]:
    h = hex_str.lstrip('#')
    if len(h) == 3:
        h = ''.join(c * 2 for c in h)
    if len(h) != 6:
        raise ValueError(f"Некорректный HEX-цвет: {hex_str}")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def remove_color_key_smart(
    img: Image.Image,
    key_rgb: tuple[int, int, int] | None,
    tolerance: int = 60,
    softness: int = 30,
    auto_corner: bool = True
) -> Image.Image:
    """
    Продвинутое удаление фона после 4K-апскейла:
    1. Если auto_corner=True, проверяет реальный цвет углов (0,0) и удаляет и его тоже (если редактор исказил HEX).
    2. Плавно срезает альфу в зоне сглаживания (softness).
    3. Вычитает паразитный оттенок фона (Despill) из полупрозрачных краев спрайта.
    """
    arr = np.array(img.convert("RGBA"), dtype=np.float32)
    rgb = arr[:, :, :3]
    alpha = arr[:, :, 3]

    # Список ключей для удаления (заданный HEX + реальный цвет из угла (0,0), если там непрозрачный фон)
    keys_to_remove = []
    if key_rgb is not None:
        keys_to_remove.append(np.array(key_rgb, dtype=np.float32))

    if auto_corner and alpha[0, 0] > 0:
        corner_rgb = rgb[0, 0].copy()
        # Добавляем цвет угла, если он еще не в списке
        if not keys_to_remove or np.linalg.norm(keys_to_remove[0] - corner_rgb) > 2.0:
            keys_to_remove.append(corner_rgb)

    if not keys_to_remove:
        return img

    for k_rgb in keys_to_remove:
        # Евклидово расстояние от каждого пикселя до цвета-ключа
        dist = np.linalg.norm(rgb - k_rgb, axis=2)

        # 1. Полное удаление внутри tolerance
        # 2. Плавный переход альфы от tolerance до (tolerance + softness)
        if softness > 0:
            factor = np.clip((dist - tolerance) / float(softness), 0.0, 1.0)
        else:
            factor = np.where(dist <= tolerance, 0.0, 1.0)

        # Применяем к текущему альфа-каналу
        alpha = alpha * factor

        # Despill: убираем цветной ореол на пограничных полупрозрачных пикселях
        edge_mask = (factor > 0.0) & (factor < 1.0)
        if np.any(edge_mask):
            # Вытягиваем цвет от фонового ключа обратно к чистому цвету спрайта
            for c in range(3):
                fg_c = (rgb[:, :, c] - (1.0 - factor) * k_rgb[c]) / np.maximum(factor, 0.05)
                rgb[:, :, c] = np.where(edge_mask, np.clip(fg_c, 0.0, 255.0), rgb[:, :, c])

    # Полностью обнуляем RGB там, где альфа стала 0
    zero_alpha = alpha < 1.0
    rgb[zero_alpha] = 0.0
    alpha[zero_alpha] = 0.0

    arr[:, :, :3] = rgb
    arr[:, :, 3] = alpha
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA")


def fill_alpha_with_key(img: Image.Image, key_rgb: tuple[int, int, int]) -> Image.Image:
    arr = np.array(img.convert("RGBA"))
    mask = arr[:, :, 3] < 128
    arr[mask] = [key_rgb[0], key_rgb[1], key_rgb[2], 255]
    return Image.fromarray(arr, "RGBA")


def pack_atlas(
    input_paths: list[str],
    output_image: str,
    output_meta: str,
    padding: int = 2,
    max_width: int = 0,
    color_key: str | None = None,
    key_to_alpha: bool = False,
    tolerance: int = 40
):
    files: list[tuple[Path, str]] = []
    for p_str in input_paths:
        p = Path(p_str)
        if p.is_dir():
            for ext in ("*.png", "*.bmp", "*.tga", "*.webp", "*.jpg", "*.jpeg"):
                for f in sorted(p.rglob(ext)):
                    files.append((f, str(f.relative_to(p))))
        elif p.is_file():
            files.append((p, p.name))

    if not files:
        raise RuntimeError("Не найдено изображений для упаковки.")

    key_rgb = hex_to_rgb(color_key) if color_key else None
    loaded = []
    total_area = 0
    max_single_w = 0

    for full_path, rel_name in files:
        img = Image.open(full_path).convert("RGBA")
        if key_rgb is not None:
            if key_to_alpha:
                img = remove_color_key_smart(img, key_rgb, tolerance=tolerance, softness=15, auto_corner=False)
            else:
                img = fill_alpha_with_key(img, key_rgb)
        w, h = img.size
        loaded.append({"name": rel_name, "img": img, "w": w, "h": h})
        total_area += (w + padding) * (h + padding)
        if w + padding > max_single_w:
            max_single_w = w + padding

    loaded.sort(key=lambda item: item["h"], reverse=True)

    if max_width <= 0:
        max_width = max(max_single_w, int(math.ceil(math.sqrt(total_area))))

    cur_x, cur_y, shelf_h = 0, 0, 0
    atlas_w = 0
    placements = []

    for item in loaded:
        w, h = item["w"], item["h"]
        if cur_x > 0 and cur_x + w > max_width:
            cur_x = 0
            cur_y += shelf_h + padding
            shelf_h = 0

        item["x"] = cur_x
        item["y"] = cur_y
        placements.append(item)

        cur_x += w + padding
        if h > shelf_h:
            shelf_h = h
        if cur_x - padding > atlas_w:
            atlas_w = cur_x - padding

    atlas_h = cur_y + shelf_h

    bg_color = (*key_rgb, 255) if (key_rgb and not key_to_alpha) else (0, 0, 0, 0)
    atlas_img = Image.new("RGBA", (atlas_w, atlas_h), bg_color)

    frames_meta = {}
    for item in placements:
        atlas_img.paste(item["img"], (item["x"], item["y"]))
        frames_meta[item["name"]] = {
            "x": item["x"],
            "y": item["y"],
            "w": item["w"],
            "h": item["h"]
        }

    frames_meta = dict(sorted(frames_meta.items(), key=lambda x: x[0]))

    meta_data = {
        "atlas_file": Path(output_image).name,
        "size": {"w": atlas_w, "h": atlas_h},
        "padding": padding,
        "color_key": color_key.upper() if color_key else None,
        "frames": frames_meta
    }

    Path(output_image).parent.mkdir(parents=True, exist_ok=True)
    atlas_img.save(output_image)

    Path(output_meta).parent.mkdir(parents=True, exist_ok=True)
    with open(output_meta, "w", encoding="utf-8") as f:
        json.dump(meta_data, f, indent=2, ensure_ascii=False)

    print(f"[PACK] Собрано: {output_image} ({atlas_w}x{atlas_h})")


def unpack_atlas(
    atlas_path: str,
    meta_path: str,
    out_dir: str,
    color_key_override: str | None = None,
    key_mode: str = "to_alpha",
    tolerance: int = 65,
    softness: int = 35,
    auto_corner: bool = True,
    manual_scale: float | None = None,
    restore_original_size: bool = False
):
    with open(meta_path, "r", encoding="utf-8") as f:
        meta = json.load(f)

    atlas = Image.open(atlas_path).convert("RGBA")
    actual_w, actual_h = atlas.size

    orig_size = meta.get("size", {"w": actual_w, "h": actual_h})
    orig_w, orig_h = orig_size["w"], orig_size["h"]

    if manual_scale is not None:
        scale_x = scale_y = manual_scale
    else:
        scale_x = actual_w / orig_w
        scale_y = actual_h / orig_h

    active_key_hex = color_key_override or meta.get("color_key")
    key_rgb = hex_to_rgb(active_key_hex) if active_key_hex else None

    # Диагностика в консоль: какой цвет реально находится в левом верхнем углу (0,0) атласа
    top_left_px = atlas.getpixel((0, 0))
    print(f"[INFO] Разрешение атласа: {actual_w}x{actual_h} (Масштаб: X={scale_x:.2f}, Y={scale_y:.2f})")
    print(f"[INFO] Пиксель (0,0) в 4K-атласе: RGBA={top_left_px} (HEX: #{top_left_px[0]:02X}{top_left_px[1]:02X}{top_left_px[2]:02X})")
    print(f"[INFO] Режим ключа: {key_mode}, Заданный HEX: {active_key_hex}, Авто-захват угла: {auto_corner}, Допуск: {tolerance}")

    out_base = Path(out_dir)
    out_base.mkdir(parents=True, exist_ok=True)

    frames = meta.get("frames", {})
    for rel_name, rect in frames.items():
        orig_fw, orig_fh = rect["w"], rect["h"]

        x = int(round(rect["x"] * scale_x))
        y = int(round(rect["y"] * scale_y))
        w = int(round(orig_fw * scale_x))
        h = int(round(orig_fh * scale_y))

        sub_img = atlas.crop((x, y, x + w, y + h))

        if key_mode == "to_alpha":
            sub_img = remove_color_key_smart(
                sub_img,
                key_rgb=key_rgb,
                tolerance=tolerance,
                softness=softness,
                auto_corner=auto_corner
            )
        elif key_mode == "from_alpha" and key_rgb is not None:
            sub_img = fill_alpha_with_key(sub_img, key_rgb)

        if restore_original_size and (w != orig_fw or h != orig_fh):
            sub_img = sub_img.resize((orig_fw, orig_fh), Image.Resampling.LANCZOS)

        dest_path = out_base / rel_name
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        sub_img.save(dest_path)

    print(f"[UNPACK] Готово! Извлечено файлов: {len(frames)} в '{out_dir}'")


def main():
    parser = argparse.ArgumentParser(description="Упаковщик/распаковщик 4K-атласов с умным удалением Color Key")
    sub = parser.add_subparsers(dest="cmd", required=True)

    # PACK
    p_pack = sub.add_parser("pack")
    p_pack.add_argument("inputs", nargs="+")
    p_pack.add_argument("-o", "--output", default="atlas.png")
    p_pack.add_argument("-m", "--meta", default="atlas.json")
    p_pack.add_argument("-p", "--padding", type=int, default=2)
    p_pack.add_argument("-w", "--max-width", type=int, default=0)
    p_pack.add_argument("-k", "--color-key", type=str, default=None)
    p_pack.add_argument("--key-to-alpha", action="store_true")
    p_pack.add_argument("-t", "--tolerance", type=int, default=40)

    # UNPACK (теперь по умолчанию сразу удаляет фон в прозрачность!)
    p_unpack = sub.add_parser("unpack")
    p_unpack.add_argument("atlas", help="Путь к 4K-атласу")
    p_unpack.add_argument("meta", help="Путь к JSON")
    p_unpack.add_argument("-d", "--out-dir", default="unpacked_sprites")
    p_unpack.add_argument("-k", "--color-key", type=str, default=None, help="HEX цвет-ключ (например '#FF00FF')")
    p_unpack.add_argument("--key-mode", choices=["to_alpha", "from_alpha", "keep"], default="to_alpha")
    p_unpack.add_argument("-t", "--tolerance", type=int, default=65, help="Жесткий радиус удаления цвета (по умолчанию 65)")
    p_unpack.add_argument("-s", "--softness", type=int, default=35, help="Мягкий радиус сглаживания бахромы (по умолчанию 35)")
    p_unpack.add_argument("--no-auto-corner", action="store_true", help="Отключить автоопределение цвета фона по углу (0,0)")
    p_unpack.add_argument("--scale", type=float, default=None)
    p_unpack.add_argument("--restore-size", action="store_true")

    args = parser.parse_args()

    if args.cmd == "pack":
        pack_atlas(
            args.inputs, args.output, args.meta,
            args.padding, args.max_width, args.color_key,
            args.key_to_alpha, args.tolerance
        )
    elif args.cmd == "unpack":
        unpack_atlas(
            atlas_path=args.atlas,
            meta_path=args.meta,
            out_dir=args.out_dir,
            color_key_override=args.color_key,
            key_mode=args.key_mode,
            tolerance=args.tolerance,
            softness=args.softness,
            auto_corner=not args.no_auto_corner,
            manual_scale=args.scale,
            restore_original_size=args.restore_size
        )


if __name__ == "__main__":
    main()

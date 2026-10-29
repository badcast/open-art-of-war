#!/usr/bin/env python3
"""
Open Art Of War: Freedom for Liberty - Visual Asset Gallery & Classification Catalog
Exports rendered PNG sprites for all buildings, units, decors, projectiles, and FX into view/,
and generates view/meta.csv, view/meta.txt, and view/index.html with detailed Russian classification.

FACTION COLOR CONVENTION (AUTHENTIC J2ME):
- КОНФЕДЕРАЦИЯ (Confederation) = СИНИЕ (Blue, Sheet 5 for buildings 0..7, Sheet 8 for units)
- СОПРОТИВЛЕНИЕ (Resistance)  = КРАСНЫЕ (Red, Sheet 6 for buildings 8..14, Sheet 9 for units)
"""

import os
import csv
import json
import shutil
from PIL import Image

VIEW_DIR = "view"
ASSETS_DIR = "assets"

def setup():
    # Clean previous run images if existing
    if os.path.exists(VIEW_DIR):
        for f in os.listdir(VIEW_DIR):
            fp = os.path.join(VIEW_DIR, f)
            if os.path.isfile(fp) and f.endswith(".png"):
                os.remove(fp)
    os.makedirs(VIEW_DIR, exist_ok=True)

def load_data():
    with open(os.path.join(ASSETS_DIR, "characters", "sprites.json"), "r", encoding="utf-8") as f:
        sprites = json.load(f)
    with open(os.path.join(ASSETS_DIR, "data", "graphics_data.json"), "r", encoding="utf-8") as f:
        graphics = json.load(f)

    sheets = {}
    for i in range(42):
        sheet_path = os.path.join(ASSETS_DIR, "sprites", f"sheet_{i:02d}.png")
        if os.path.exists(sheet_path):
            sheets[i] = Image.open(sheet_path).convert("RGBA")

    return sprites, graphics, sheets

def make_renderer(sprites, graphics, sheets):
    comp_ranges = graphics["composite_sprites"]["ranges"]
    comp_parts = graphics["composite_sprites"]["parts"]
    anim_ranges = graphics["anim_sequences"]["ranges"]
    anim_frames = graphics["anim_sequences"]["frames"]
    b_ranges = graphics["buildings"]["ranges"]
    b_parts = graphics["buildings"]["parts"]
    b_widths = graphics["buildings"]["widths"]
    b_heights = graphics["buildings"]["heights"]
    unit_archetypes = graphics["unit_archetypes"]
    seq_ranges = graphics["unit_sequences"]["ranges"]
    seq_steps = graphics["unit_sequences"]["steps"]
    body_ranges = graphics["unit_bodies"]["ranges"]
    body_layers = graphics["unit_bodies"]["layers"]

    def collect_ops(kind, item_id, x, y, is_resistance=False, depth=0, ops=None):
        if ops is None:
            ops = []
        if depth > 10:
            return ops

        if kind == "raw":
            if 0 <= item_id < len(sprites):
                s = sprites[item_id]
                if s["width"] > 0 and s["height"] > 0:
                    page = s["page"]
                    # If Resistance (Red), switch base sheet 8 to red sheet 9
                    if page == 8 and is_resistance:
                        page = 9
                    ops.append((page, s["x"], s["y"], s["width"], s["height"], x, y))
        elif kind == "comp":
            if 0 <= item_id < len(comp_ranges):
                st, en = comp_ranges[item_id]
                if st <= en:
                    for p in comp_parts[st:en + 1]:
                        ckind = "comp" if p["is_comp"] == 1 else "raw"
                        collect_ops(ckind, p["child"], x + p["ox"], y + p["oy"], is_resistance, depth + 1, ops)
        elif kind == "anim":
            if 0 <= item_id < len(anim_ranges):
                st, en = anim_ranges[item_id]
                if st <= en:
                    f = anim_frames[st]
                    akind = "comp" if f["is_comp"] == 1 else "raw"
                    collect_ops(akind, f["child"], x + f["ox"], y + f["oy"], is_resistance, depth + 1, ops)
        return ops

    def render_ops_image(ops, margin=2):
        if not ops:
            return Image.new("RGBA", (16, 16), (0, 0, 0, 0))

        min_x = min(x for _, _, _, _, _, x, _ in ops)
        min_y = min(y for _, _, _, _, _, _, y in ops)
        max_x = max(x + w for _, _, _, w, _, x, _ in ops)
        max_y = max(y + h for _, _, _, _, h, _, y in ops)

        w = max(1, max_x - min_x + margin * 2)
        h = max(1, max_y - min_y + margin * 2)

        base_img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        for page, sx, sy, sw, sh, dx, dy in ops:
            if page in sheets:
                cropped = sheets[page].crop((sx, sy, sx + sw, sy + sh))
                dest_x = dx - min_x + margin
                dest_y = dy - min_y + margin
                base_img.alpha_composite(cropped, (dest_x, dest_y))

        return base_img

    def render_building(b_id):
        if b_id < 0 or b_id >= len(b_ranges):
            return Image.new("RGBA", (16, 16), (0, 0, 0, 0))

        is_res = (8 <= b_id <= 14)
        st, en = b_ranges[b_id]
        ops = []
        if st <= en:
            for p in b_parts[st:en + 1]:
                if p.get("state", 0) in (0, 1, 2):
                    k = "raw" if p["is_comp"] == 0 else ("comp" if p["is_comp"] == 1 else "anim")
                    collect_ops(k, p["child"], p["ox"], p["oy"], is_resistance=is_res, ops=ops)
            if not ops:
                for p in b_parts[st:en + 1]:
                    k = "raw" if p["is_comp"] == 0 else ("comp" if p["is_comp"] == 1 else "anim")
                    collect_ops(k, p["child"], p["ox"], p["oy"], is_resistance=is_res, ops=ops)

        return render_ops_image(ops)

    def render_unit(arch_id, is_resistance=False, facing=2):
        if arch_id < 0 or arch_id >= len(unit_archetypes):
            return Image.new("RGBA", (16, 16), (0, 0, 0, 0))

        base_seq = unit_archetypes[arch_id][0]
        seq_idx = base_seq + facing
        if seq_idx >= len(seq_ranges):
            seq_idx = base_seq

        st, en = seq_ranges[seq_idx]
        ops = []
        if st <= en:
            step = seq_steps[st]
            body_id = step["body"]
            if body_id < len(body_ranges):
                bst, ben = body_ranges[body_id]
                for lay in body_layers[bst:ben + 1]:
                    k = "raw" if lay["is_comp"] == 0 else ("comp" if lay["is_comp"] == 1 else "anim")
                    collect_ops(k, lay["child"], step["ox"] + lay["ox"], step["oy"] + lay["oy"], is_resistance=is_resistance, ops=ops)

        return render_ops_image(ops)

    def render_composite(comp_id, is_resistance=False):
        ops = []
        collect_ops("comp", comp_id, 0, 0, is_resistance=is_resistance, ops=ops)
        return render_ops_image(ops)

    def render_anim(anim_id, is_resistance=False):
        ops = []
        collect_ops("anim", anim_id, 0, 0, is_resistance=is_resistance, ops=ops)
        return render_ops_image(ops)

    return render_building, render_unit, render_composite, render_anim, b_widths, b_heights, graphics

def classify_and_generate():
    setup()
    sprites, graphics, sheets = load_data()
    render_b, render_u, render_c, render_a, b_w, b_h, g = make_renderer(sprites, graphics, sheets)

    catalog = []
    item_no = 1

    # -------------------------------------------------------------
    # 1. Base Buildings: Confederation (0..7, Blue) & Resistance (8..14, Red)
    # -------------------------------------------------------------
    conf_buildings = [
        (0, "building_00_conf_command_center.png", "Командный центр",
         "Главный командный штаб сил Конфедерации (синяя регулярная армия). Центр управления войсками и координации операций."),
        (1, "building_01_conf_infantry_center.png", "Пехотный центр",
         "Казармы регулярной пехоты Конфедерации: подготовка штурмовиков, тяжелых штурмовиков и огнеметчиков."),
        (2, "building_02_conf_vehicle_factory.png", "Завод техники",
         "Тяжелый танковый завод Конфедерации: производство танков Т-21 Молот, Т-22 Зевс, сверхтяжелых ТМ-40 Крепость, РСЗО Град."),
        (3, "building_03_conf_power_generator.png", "Генератор",
         "Тяжелая электростанция-генератор Конфедерации, обеспечивает энергией высокотехнологичные базы и радары."),
        (4, "building_04_conf_radar_locator.png", "Локатор / Вышка",
         "Радиолокационная сторожевая вышка Конфедерации с системой кругового обнаружения и легким орудием."),
        (5, "building_05_conf_tech_center.png", "Центр технологий",
         "Научно-исследовательский комплекс передовых военных разработок и тяжелого бронирования Конфедерации."),
        (6, "building_06_conf_bunker.png", "Бункер",
         "Укрепленный стационарный дот Конфедерации для размещения гарнизона и ведения огня из укрытия."),
        (7, "building_07_conf_missile_turret.png", "Ракетная установка",
         "Базовая ракетная турель ПВО и наземной обороны Конфедерации высокой дальности поражения.")
    ]

    for b_id, fname, name_ru, desc in conf_buildings:
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Здание",
            "category": "Конфедерация (Синие)",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    res_buildings = [
        (8, "building_08_res_headquarters.png", "Штаб",
         "Главный командный центр партизан Сопротивления (красная повстанческая армия Перу). Обеспечивает снабжение и организацию баз."),
        (9, "building_09_res_barracks.png", "Казарма",
         "Полевой лагерь подготовки бойцов Сопротивления: пехотинцы, гренадеры, снайперы, ракетчики."),
        (10, "building_10_res_factory.png", "Завод",
         "Машиностроительный завод партизан: сборка броневиков Койот, танков Броненосец, Носорог и установок Дикобраз."),
        (11, "building_11_res_powerplant.png", "Электростанция",
         "Электростанция Сопротивления, снабжает энергией все оборонительные и промышленные сооружения повстанцев."),
        (12, "building_12_res_guard_tower.png", "Вышка",
         "Сторожевая вышка Сопротивления с пулеметной точкой и сектором обзора скрытых подходов к базе."),
        (13, "building_13_res_tech_lab.png", "Лаборатория",
         "Подпольная научно-техническая лаборатория Сопротивления для модификации вооружения и диверсий."),
        (14, "building_14_res_bunker.png", "Бункер",
         "Бетонное укрытие и пулеметный дзот партизанского сопротивления высокой прочности.")
    ]

    for b_id, fname, name_ru, desc in res_buildings:
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Здание",
            "category": "Сопротивление (Красные)",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 2. Base Defense Walls, Barricades, Obstacles (15..29)
    # -------------------------------------------------------------
    fort_desc = [
        (15, "building_15_wall_concrete_h.png", "Стена бетонная (горизонтальная)", "Оборонительное укрепление из монолитного железобетона, блокирует продвижение техники и пехоты."),
        (16, "building_16_wall_concrete_h_damaged.png", "Стена бетонная поврежденная", "Секция бетонной стены с частичными трещинами и боевыми повреждениями."),
        (17, "building_17_wall_concrete_h_breached.png", "Стена бетонная пробитая", "Секция стены с глубоким сквозным пробоем от артиллерийского снаряда."),
        (18, "building_18_wall_concrete_v.png", "Стена бетонная (вертикальная)", "Вертикальная секция железобетонного защитного периметра базы."),
        (19, "building_19_wall_concrete_v_damaged.png", "Стена бетонная вертикальная поврежденная", "Вертикальная стена с повреждениями защитного покрытия."),
        (20, "building_20_wall_reinforced_h.png", "Стена усиленная бронированная (горизонтальная)", "Тяжелая укрепленная фортификационная стена повышенной прочности."),
        (21, "building_21_wall_reinforced_h_damaged.png", "Стена усиленная поврежденная", "Тяжелая стена со следами взрывов и осколков."),
        (22, "building_22_wall_reinforced_h_breached.png", "Стена усиленная разрушенная", "Разбитый сегмент тяжелой оборонительной стены."),
        (23, "building_23_wall_reinforced_v.png", "Стена усиленная (вертикальная)", "Вертикальный усиленный бронебарьер периметра."),
        (24, "building_24_wall_reinforced_v_damaged.png", "Стена усиленная вертикальная поврежденная", "Вертикальный барьер со следами артобстрела."),
        (25, "building_25_barricade_sandbags.png", "Бруствер из мешков с песком", "Окопное укрепление из мешков с песком для прикрытия пехоты и огневых точек."),
        (26, "building_26_barricade_sandbags_damaged.png", "Песчаный бруствер поврежденный", "Разбитый пехотный бруствер после стрелкового боя."),
        (27, "building_27_razor_wire.png", "Заграждение из колючей проволоки", "Инженерное заграждение из спирали Бруно, задерживает пехоту противника."),
        (28, "building_28_checkpoint_barrier.png", "Шлагбаум контрольно-пропускного пункта", "Шлагбаум и защитный пост на въезде на военную базу."),
        (29, "building_29_roadblock_ruins.png", "Дорожный завал / баррикада", "Импровизированный завал на дороге из бетонных блоков и обломков.")
    ]

    for b_id, fname, name_ru, desc in fort_desc:
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Здание / Укрепление",
            "category": "Окружение (Укрепления)",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 3. City, Village & Industrial Buildings (30..54)
    # -------------------------------------------------------------
    for b_id in range(30, 55):
        fname = f"building_{b_id:02d}_civic_structure.png"
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"

        if b_id in (30, 31, 32, 33):
            sub_type = "Городской жилой сектор Лимы"
            det = "Двухэтажные жилые и административные здания перуанского города."
        elif b_id in (34, 35, 36, 37):
            sub_type = "Складской комплекс / терминал"
            det = "Промышленные склады для хранения продовольствия, оборудования и ресурсов."
        elif b_id in (38, 39, 40, 41):
            sub_type = "Топливный терминал / насосная станция"
            det = "Индустриальные резервуары и насосные узлы энергоснабжения региона."
        elif b_id in (42, 43, 44, 45):
            sub_type = "Городская больница / правительственное здание"
            det = "Крупные общественные сооружения с внутренними дворами и ограждениями."
        elif b_id in (46, 47, 48):
            sub_type = "Сельские постройки / база повстанцев"
            det = "Деревянно-глинобитные постройки и усадьбы в предгорьях Анд."
        elif b_id in (49, 50):
            sub_type = "Крупный войсковой ангар / цех сборки (3x3)"
            det = "Массивное промышленное здание для стоянки тяжелой техники и ремонта."
        else:
            sub_type = "Военно-индустриальный модуль связи / лаборатория (2x2)"
            det = "Специализированные вспомогательные военные объекты на территории баз."

        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Здание",
            "category": "Окружение (Инфраструктура)",
            "size": size_str,
            "description": f"{sub_type} — {det}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 4. Infrastructure, Poles, Barrels, Checkpoints, Wrecks (55..82)
    # -------------------------------------------------------------
    for b_id in range(55, 83):
        fname = f"building_{b_id:02d}_infra_decor.png"
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"

        if 55 <= b_id <= 61:
            desc = "Опора высоковольтной ЛЭП / телеграфный столб — элемент энергетической инфраструктуры карты."
        elif 62 <= b_id <= 66:
            desc = "Осветительная вышка с прожектором — освещает периметр военных объектов и баз ночью."
        elif 67 <= b_id <= 74:
            desc = "Топливные цистерны и склад бочек ГСМ — взрывоопасные объекты снабжения горючим."
        elif 75 <= b_id <= 78:
            desc = "Противотанковые надолбы / бетонные блоки — блокируют продвижение колесной и гусеничной техники."
        else:
            desc = "Остов сгоревшей бронетехники / военный металлолом — неразрушимое укрытие на поле боя."

        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Объект (Декорация)",
            "category": "Окружение (Инфраструктура)",
            "size": size_str,
            "description": f"Элемент карты {b_id} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 5. Bridges & Water Crossings (83..108)
    # -------------------------------------------------------------
    for b_id in range(83, 109):
        fname = f"building_{b_id:02d}_bridge_crossing.png"
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"

        if 83 <= b_id <= 86:
            desc = "Широкий автомобильный мост через реку (деревянный/стальной) — ключевая стратегическая переправа."
        elif 87 <= b_id <= 96:
            desc = "Секция моста / пандус съезда в воду — обеспечивает проезд бронетехники через водные преграды."
        elif 97 <= b_id <= 104:
            desc = "Причал / пирс для речных катеров и переправы грузов в тропических реках Перу."
        else:
            desc = "Пешеходный мостик / настил через горные ущелья и каньоны."

        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Здание / Переправа",
            "category": "Окружение (Инфраструктура)",
            "size": size_str,
            "description": f"Мостовая переправа {b_id} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 6. Artillery Fortifications, Trenches & Platforms (109..120)
    # -------------------------------------------------------------
    for b_id in range(109, 121):
        fname = f"building_{b_id:02d}_fortification.png"
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"

        if 109 <= b_id <= 112:
            desc = "Окопная артиллерийская позиция — железобетонный капонир для установки стационарных орудий."
        elif 113 <= b_id <= 116:
            desc = "Траншейный узел пехоты — система укрепленных окопов для долговременной обороны."
        else:
            desc = "Бетонированная вертолетная площадка / пандус для обслуживания боевой техники."

        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Здание / Укрепление",
            "category": "Окружение (Укрепления)",
            "size": size_str,
            "description": f"Фортификационный узел {b_id} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 7. Special Mission Structures, Oil Wells, Inca Ruins (121..132)
    # -------------------------------------------------------------
    for b_id in range(121, 133):
        fname = f"building_{b_id:02d}_special_landmark.png"
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"

        if 121 <= b_id <= 124:
            desc = "Нефтяная качалка / вышка добычи ресурсов — источник сырья и финансирования войск."
        elif 125 <= b_id <= 128:
            desc = "Древний перуанский храм / руины Инков — исторические памятники в высокогорье Анд (сюжетные цели)."
        else:
            desc = "Спутниковый узел дальней связи / ретранслятор Конфедерации (цель диверсионных миссий)."

        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Здание / Спецобъект",
            "category": "Окружение (Спецобъекты)",
            "size": size_str,
            "description": f"Сюжетный объект {b_id} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 8. Tactical In-Game HUD Frames & Dialog Windows (133..145)
    # -------------------------------------------------------------
    for b_id in range(133, 146):
        fname = f"building_{b_id:02d}_tactical_ui_frame.png"
        img = render_b(b_id)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"{b_w[b_id]}x{b_h[b_id]} ({img.width}x{img.height} px)"

        if b_id == 133:
            desc = "Главная панель контекстных приказов тактического меню (атака, движение, ремонт)."
        elif b_id == 134:
            desc = "Горизонтальная статусная рамка ресурсов, таймера и сообщений штаба."
        elif b_id == 145:
            desc = "Рамка всплывающего внутриигрового меню паузы, сохранения и целей миссии."
        else:
            desc = "Тактическое диалоговое окно / панель выбора зданий для строительства."

        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Интерфейс",
            "category": "UI / Меню",
            "size": size_str,
            "description": f"Элемент тактического интерфейса {b_id} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 9. Playable Units: Confederation (Blue) & Resistance (Red)
    # -------------------------------------------------------------
    conf_units = [
        (0, "unit_00_conf_rifleman.png", "Штурмовик",
         "Регулярный боец пехотных частей Конфедерации в бронежилете с автоматическим оружием (синяя палитра Sheet 8)."),
        (1, "unit_01_conf_heavy_assault.png", "Тяж. штурмовик",
         "Штурмовик Конфедерации в экзоброне с ракетометом, предназначен для штурма баз и бронетехники."),
        (4, "unit_02_conf_flamethrower.png", "Огнеметчик",
         "Специалист по зачистке траншей, бункеров и лесных массивов, наносит огромный урон по площади огнеметом."),
        (12, "unit_03_conf_fortress.png", "ТМ-40 Крепость",
         "Сверхтяжелый гусеничный танк Конфедерации с максимальной бронезащитой на поле боя."),
        (9, "unit_04_conf_hammer.png", "Т-21 Молот",
         "Основной боевой танк Конфедерации с автоматической пушкой и высокой скоростью атаки."),
        (10, "unit_05_conf_zeus.png", "Т-22 Зевс",
         "Элитный тяжелый танк Конфедерации со спаренным орудием высокой бронепробиваемости."),
        (11, "unit_06_conf_grad.png", "РСЗО Град",
         "Дальнобойная колесная реактивная система артиллерийского подавления укреплений.")
    ]

    for arch_id, fname, name_ru, desc in conf_units:
        img = render_u(arch_id, is_resistance=False)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"1x1 ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Юнит",
            "category": "Конфедерация (Синие)",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    res_units = [
        (0, "unit_07_res_rifleman.png", "Пехотинец",
         "Легкая базовая пехота Сопротивления, вооружен автоматической винтовкой (красная партизанская палитра Sheet 9)."),
        (1, "unit_08_res_rocket_soldier.png", "Ракетчик",
         "Пехотинец с ручным гранатометом/ПТУР, основная противотанковая сила пехоты Сопротивления."),
        (2, "unit_09_res_grenadier.png", "Гренадер",
         "Тяжелый штурмовик, метает фугасные гранаты по скоплениям пехоты и укрепленным огневым точкам."),
        (5, "unit_10_res_sniper.png", "Снайпер",
         "Элитный боец с высокой точностью и дальностью поражения, уничтожает вражеских солдат до их сближения."),
        (6, "unit_11_res_coyote.png", "Койот",
         "Скоростной легкий разведывательный багги Сопротивления с пулеметной турелью, мобильный дозор."),
        (7, "unit_12_res_armadillo.png", "Броненосец",
         "Средний штурмовой танк Сопротивления со сбалансированной броней и скорострельной пушкой."),
        (8, "unit_13_res_rhino.png", "Носорог",
         "Тяжелый танк прорыва с усиленной лобовой броней и мощным крупнокалиберным орудием."),
        (11, "unit_14_res_porcupine.png", "Дикобраз",
         "Самоходная реактивная система залпового огня Сопротивления, ведет разрушительный огонь по площадям.")
    ]

    for arch_id, fname, name_ru, desc in res_units:
        img = render_u(arch_id, is_resistance=True)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"1x1 ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Юнит",
            "category": "Сопротивление (Красные)",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 10. Environmental Decors & Animated Landmarks
    # -------------------------------------------------------------
    decors_meta = [
        ("decor_01_palm_tree.png", 87, "Пальма джунглей", "Тропическая кокосовая пальма перуанской сельвы, декорация карты."),
        ("decor_02_jungle_tree.png", 88, "Дерево джунглей", "Широколиственное тропическое дерево, формирует непроходимые массивы джунглей."),
        ("decor_03_rock_boulder.png", 91, "Скальный валун", "Природный массив гранита, непроходимое естественное препятствие на карте."),
        ("decor_04_rock_cluster.png", 92, "Каменная гряда", "Гряда камней и скал в высокогорных районах Перу."),
        ("decor_05_ruins_column.png", 96, "Руины колонны", "Обломок древней каменной колонны доколумбовой эпохи."),
        ("decor_06_supply_crate.png", 100, "Ящик с припасами", "Сброшенный военный контейнер с боеприпасами и ресурсами."),
        ("decor_07_crater.png", 103, "Воронка от взрыва", "Кратер в грунте после детонации тяжелого артиллерийского снаряда.")
    ]

    for fname, cid, name_ru, desc in decors_meta:
        img = render_c(cid, is_resistance=False)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"Декор ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Объект (Декорация)",
            "category": "Окружение (Джунгли/Горы)",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    anim_decors_meta = [
        ("decor_anim_01_campfire.png", 1741, "Костер / Сигнальный огонь", "Анимированное пламя партизанского костра или сигнального огня."),
        ("decor_anim_02_oil_burner.png", 1747, "Факел нефтедобычи", "Анимированный газовый факел на вышке сгорания нефтяных газов."),
        ("decor_anim_03_smoke_vent.png", 1753, "Дымовой клапан", "Анимированный клуб пара/дыма вентиляционной шахты завода."),
        ("decor_anim_04_water_splash.png", 1759, "Водный бурун", "Анимированные брызги воды на речных порогах и перекатах."),
        ("decor_anim_05_radar_dish.png", 1764, "Вращающийся радар", "Анимированная антенна радара раннего предупреждения."),
        ("decor_anim_06_generator_spark.png", 1773, "Электрическая дуга", "Анимированные разряды тока на трансформаторах электростанции.")
    ]

    for fname, cid, name_ru, desc in anim_decors_meta:
        img = render_c(cid, is_resistance=False)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"Анимация ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": "Объект (Анимация)",
            "category": "Окружение (Инфраструктура)",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # 11. Projectiles & Combat FX
    # -------------------------------------------------------------
    fx_meta = [
        ("fx_01_bullet_tracer.png", "comp", 1309, "Трассер пули", "Спецэффект / Снаряд", "Огонь стрелкового оружия пехоты и пулеметов Койота."),
        ("fx_02_tank_shell.png", "comp", 1341, "Танковый снаряд", "Спецэффект / Снаряд", "Бронебойный снаряд пушек танков Броненосец, Носорог, Молот."),
        ("fx_03_rocket_projectile.png", "comp", 1405, "Ракета РСЗО / ПТУР", "Спецэффект / Снаряд", "Реактивный неуправляемый снаряд РСЗО Град / Дикобраз."),
        ("fx_04_explosion_small.png", "anim", 11, "Взрыв снаряда (малый)", "Спецэффект", "Анимированный взрыв при попадании пули и гранаты."),
        ("fx_05_explosion_medium.png", "anim", 12, "Взрыв техники (средний)", "Спецэффект", "Анимированная детонация легкой техники и ракет."),
        ("fx_06_explosion_large.png", "anim", 18, "Взрыв здания (крупный)", "Спецэффект", "Массивный огненный шар при уничтожении зданий и тяжелых танков."),
        ("fx_07_building_rubble.png", "comp", 2174, "Руины / Обломки здания", "Объект (Декорация)", "Обгоревшие бетонные плиты и воронка на месте разрушенного здания.")
    ]

    for fname, rtype, rid, name_ru, cat, desc in fx_meta:
        if rtype == "comp":
            img = render_c(rid, is_resistance=False)
        else:
            img = render_a(rid, is_resistance=False)
        img.save(os.path.join(VIEW_DIR, fname))
        size_str = f"FX ({img.width}x{img.height} px)"
        catalog.append({
            "no": item_no,
            "sprite filename": fname,
            "class": cat,
            "category": "Боевые эффекты",
            "size": size_str,
            "description": f"{name_ru} — {desc}"
        })
        item_no += 1

    # -------------------------------------------------------------
    # Write meta.csv (Preserve existing 8-column user-verified CSV if present)
    # -------------------------------------------------------------
    csv_path = os.path.join(VIEW_DIR, "meta.csv")
    has_user_meta = False
    if os.path.exists(csv_path):
        with open(csv_path, "r", encoding="utf-8-sig") as f:
            first_line = f.readline()
            if "user_status" in first_line:
                has_user_meta = True

    if not has_user_meta:
        with open(csv_path, "w", encoding="utf-8-sig", newline="") as f:
            writer = csv.writer(f, delimiter=";")
            writer.writerow(["no", "sprite filename", "class", "category", "size", "description", "user_status", "user_note"])
            for row in catalog:
                writer.writerow([
                    row["no"],
                    row["sprite filename"],
                    row["class"],
                    row["category"],
                    row["size"],
                    row["description"],
                    "none",
                    ""
                ])
        print(f"Exported fallback CSV: {csv_path} ({len(catalog)} entries)")
    else:
        print(f"Preserving existing user-verified CSV: {csv_path}")

    # -------------------------------------------------------------
    # Write meta.txt (Clean aligned human-readable text table)
    # -------------------------------------------------------------
    txt_path = os.path.join(VIEW_DIR, "meta.txt")
    with open(txt_path, "w", encoding="utf-8") as f:
        header = f"{'№':>4} | {'Имя файла спрайта':<38} | {'Класс':<20} | {'Категория':<26} | {'Размер':<18} | {'Описание'}"
        f.write("=" * 165 + "\n")
        f.write("Open Art Of War: Freedom for Liberty — ПОЛНЫЙ КАТАЛОГ ГРАФИЧЕСКИХ СПРАЙТОВ И ОБЪЕКТОВ\n")
        f.write("=" * 165 + "\n")
        f.write(header + "\n")
        f.write("-" * 165 + "\n")
        for r in catalog:
            line = f"{r['no']:4d} | {r['sprite filename']:<38} | {r['class']:<20} | {r['category']:<26} | {r['size']:<18} | {r['description']}"
            f.write(line + "\n")
        f.write("=" * 165 + "\n")
    print(f"Exported TXT: {txt_path}")

    # -------------------------------------------------------------
    # Write index.html (Interactive Visual Editor & CSV Exporter)
    # -------------------------------------------------------------
    try:
        from generate_index_html import generate_html
        generate_html()
    except Exception as e:
        import subprocess
        subprocess.run(["python3", "tools/generate_index_html.py"])
    print(f"Exported interactive HTML editor to view/index.html")

if __name__ == "__main__":
    classify_and_generate()

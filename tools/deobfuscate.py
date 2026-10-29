#!/usr/bin/env python3
"""
High-Precision Automated Deobfuscator & Refactorer for Art Of War 2 (J2ME)
Applies symbol_map.json with syntactic awareness (types vs member access vs variables),
annotates magic constants, and outputs clean Java code to decompiled_clean/.
"""

import os
import re
import json

def load_symbol_map(path="docs/artofwar2_java_map.json"):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)

def annotate_magic_constants(code):
    # Annotate unit array offsets (multiples of 101)
    unit_offsets = {
        0: "/* [Unit: tileX] */",
        101: "/* [Unit: tileY] */",
        202: "/* [Unit: subTileX (0..29)] */",
        303: "/* [Unit: subTileY (0..29)] */",
        404: "/* [Unit: targetTileX] */",
        505: "/* [Unit: targetTileY] */",
        1010: "/* [Unit: targetEntityId] */",
        1414: "/* [Unit: maxHp] */",
        1616: "/* [Unit: currentHp] */",
        2323: "/* [Unit: unitTypeId] */",
        4747: "/* [Unit: facingDirection (0..15)] */",
        4848: "/* [Unit: actionState (0:Idle, 1:Move, 2:Attack, 3:Die, 4:Build, 5:Harvest)] */",
        5454: "/* [Unit: factionId (0:Resistance, 1:Confederation)] */"
    }
    for off, desc in unit_offsets.items():
        code = re.sub(rf'\[\s*{off}\s*\+', f'[{off} {desc} +', code)

    # Annotate tile size 30
    code = re.sub(r'(\*\s*30\b)', r'\1 /* TILE_PIXELS */', code)

    # Annotate J2ME softkeys and navigation
    key_codes = {
        "-1": "/* KEY_UP */",
        "-2": "/* KEY_DOWN */",
        "-3": "/* KEY_LEFT */",
        "-4": "/* KEY_RIGHT */",
        "-5": "/* KEY_FIRE / SELECT */",
        "-6": "/* KEY_SOFT_LEFT */",
        "-7": "/* KEY_SOFT_RIGHT */",
        "-8": "/* KEY_CLEAR */"
    }
    for kc, desc in key_codes.items():
        code = re.sub(rf'\bcase\s+{re.escape(kc)}\s*:', f'case {kc} {desc}:', code)
        code = re.sub(rf'==\s*{re.escape(kc)}\b', f'== {kc} {desc}', code)

    return code

def replace_types(code, class_map):
    # Sort by descending length so 'aow22' is replaced before 'a'
    for old_cls in sorted(class_map.keys(), key=len, reverse=True):
        new_cls = class_map[old_cls]

        # 1. Class / interface declarations
        code = re.sub(rf'\b(class|interface)\s+{old_cls}\b', rf'\1 {new_cls}', code)
        code = re.sub(rf'\b(extends|implements)\s+{old_cls}\b', rf'\1 {new_cls}', code)

        # 2. Constructors
        code = re.sub(rf'\bpublic\s+{old_cls}\s*\(', f'public {new_cls}(', code)

        # 3. Instantiations: new old_cls( or new old_cls[
        code = re.sub(rf'\bnew\s+{old_cls}\s*\(', rf'new {new_cls}(', code)
        code = re.sub(rf'\bnew\s+{old_cls}\s*\[', rf'new {new_cls}[', code)

        # 4. Casts: (old_cls)
        code = re.sub(rf'\(\s*{old_cls}\s*\)', rf'({new_cls})', code)

        # 5. Method parameter / return / field types
        code = re.sub(rf'(\(\s*){old_cls}(\s+[a-zA-Z0-9_]+)', rf'\1{new_cls}\2', code)
        code = re.sub(rf'(,\s*){old_cls}(\s+[a-zA-Z0-9_]+)', rf'\1{new_cls}\2', code)
        code = re.sub(rf'\b(public|private|protected|static|final)\s+{old_cls}(\s+[a-zA-Z0-9_]+)', rf'\1 {new_cls}\2', code)
        code = re.sub(rf'\b(public|private|protected|static|final)\s+{old_cls}\[\](\s+[a-zA-Z0-9_]+)', rf'\1 {new_cls}[]\2', code)
        code = re.sub(rf'\b(public|private|protected|static|final)\s+{old_cls}\[\]\[\](\s+[a-zA-Z0-9_]+)', rf'\1 {new_cls}[][]\2', code)

        # 6. Static member access: "old_cls." when NOT preceded by "."
        code = re.sub(rf'(?<![\w\.]){old_cls}\.', rf'{new_cls}.', code)

    return code

def deobfuscate_classes(raw_dir="decompiled_raw", clean_dir="decompiled_clean", sym_map=None):
    os.makedirs(clean_dir, exist_ok=True)
    class_map = sym_map.get("classes", {})
    methods_map = sym_map.get("methods", {})

    for old_cls, new_cls in class_map.items():
        src_path = os.path.join(raw_dir, f"{old_cls}.java")
        if not os.path.exists(src_path):
            continue

        with open(src_path, "r", encoding="utf-8") as fh:
            code = fh.read()

        # 1. Type replacement
        code = replace_types(code, class_map)

        # 2. Method renames within this class definition
        if new_cls in methods_map:
            cls_methods = methods_map[new_cls]
            for old_m, new_m in cls_methods.items():
                pure_name = old_m.split('(')[0]
                # Replace method declaration
                code = re.sub(rf'\b(void|int|byte|short|boolean|String|Image|Graphics)\s+{pure_name}\s*\(', rf'\1 {new_m}(', code)

        # 3. Magic constants annotation
        code = annotate_magic_constants(code)

        # 4. Banner header
        cls_desc = sym_map.get("class_descriptions", {}).get(old_cls, "Deobfuscated Art of War 2 engine class")
        banner = f"""/**
 * ============================================================================
 * Art of War 2: Liberation of Peru - Ported & Deobfuscated
 * Target: {new_cls} (Original: {old_cls}.class)
 * Role: {cls_desc}
 * ============================================================================
 */
"""
        code = banner + "\n" + code

        out_path = os.path.join(clean_dir, f"{new_cls}.java")
        with open(out_path, "w", encoding="utf-8") as fh:
            fh.write(code)
        print(f"Deobfuscated: {old_cls}.java -> {new_cls}.java ({len(code.splitlines())} lines)")

if __name__ == "__main__":
    symbols = load_symbol_map()
    deobfuscate_classes(sym_map=symbols)

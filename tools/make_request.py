import sys
from pathlib import Path

CPP_EXTENSIONS = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".hxx", ".c"}
OUTPUT_FILE = "REQUEST.md"


def collect_files(paths: list[str]) -> list[Path]:
    collected = []
    seen = set()

    for p_str in paths:
        path = Path(p_str)
        if not path.exists():
            print(f"Предупреждение: путь '{p_str}' не существует.", file=sys.stderr)
            continue

        if path.is_file():
            resolved = path.resolve()
            if resolved not in seen:
                seen.add(resolved)
                collected.append(path)

        elif path.is_dir():
            for item in sorted(path.rglob("*")):
                if item.is_file() and item.suffix.lower() in CPP_EXTENSIONS:
                    resolved = item.resolve()
                    if resolved not in seen:
                        seen.add(resolved)
                        collected.append(item)

    return collected


def get_display_path(file_path: Path) -> str:
    """Возвращает читаемый относительный путь с прямыми слешами."""
    try:
        rel = file_path.resolve().relative_to(Path.cwd().resolve())
        return rel.as_posix()
    except ValueError:
        # Если файл лежит на другом диске или вне рабочей директории
        return file_path.as_posix()


def build_markdown(files: list[Path], output_path: str = OUTPUT_FILE) -> None:
    with open(output_path, "w", encoding="utf-8") as out:
        for file_path in files:
            try:
                content = file_path.read_text(encoding="utf-8", errors="replace")
            except Exception as e:
                print(f"Ошибка чтения {file_path}: {e}", file=sys.stderr)
                continue

            # Путь в заголовке вместо одного лишь имени файла
            display_path = get_display_path(file_path)
            out.write(f"### {display_path}#\n")
            out.write("```cpp\n")
            out.write(content.rstrip("\n") + "\n")
            out.write("```\n\n")


def main():
    if len(sys.argv) < 2:
        print("Использование: python make_request.py <файл_или_папка_1> [путь_2 ...]")
        sys.exit(1)

    files = collect_files(sys.argv[1:])
    if not files:
        print("Подходящие файлы не найдены.")
        return

    build_markdown(files)
    print(f"Готово! Обработано файлов: {len(files)} -> {OUTPUT_FILE}")


if __name__ == "__main__":
    main()

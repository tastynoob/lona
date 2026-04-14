import os

out_dir = os.environ.get("LONA_BUILD_DIR", "build")
os.makedirs(out_dir, exist_ok=True)

with open(os.path.join(out_dir, "gen.yacc"), "w") as gen:
    with open("grammar/main.yacc") as f:
        main_yacc = f.readlines()

    for line in main_yacc:
        if line.startswith("%include"):
            include_file = "grammar/" + line[9:-1].strip()
            if os.path.exists(include_file):
                with open(include_file) as f:
                    gen.write(f.read())
                    gen.write('\n')
            else:
                raise FileNotFoundError(f"File {include_file} does not exist")
        else:
            gen.write(line)

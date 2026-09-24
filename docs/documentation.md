# Documentation

The site uses MkDocs Material, with the same navigation structure, dark charcoal surfaces, blue-grey accents, theme switcher, and code-copy controls as Hammer Addons.

```powershell
python -m venv .venv-docs
.\.venv-docs\Scripts\python.exe -m pip install -r requirements-docs.txt
.\.venv-docs\Scripts\python.exe -m mkdocs build --strict
.\.venv-docs\Scripts\python.exe -m mkdocs serve --dev-addr 127.0.0.1:8000
```

The generated site is `site/`. Check both color schemes and mobile navigation when changing the theme. Keep installation status explicit and distinguish tested features from planned APIs.

Public pages address players, mod authors, and contributors. Local binary-analysis reports belong in the ignored `.local/` directory. Do not publish game binaries or personal installation details as project assets.

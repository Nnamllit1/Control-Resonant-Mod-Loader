# Documentation

The site uses MkDocs Material, with the same navigation structure, dark charcoal surfaces, blue-grey accents, theme switcher, and code-copy controls as Hammer Addons.

```powershell
python -m venv .venv-docs
.\.venv-docs\Scripts\python.exe -m pip install -r requirements-docs.txt
.\.venv-docs\Scripts\python.exe -m mkdocs build --strict
.\.venv-docs\Scripts\python.exe -m mkdocs serve --dev-addr 127.0.0.1:8000
```

The generated site is `site/`. Check both color schemes and mobile navigation when changing the theme. Keep installation status explicit and distinguish tested features from planned APIs.

## GitHub Pages

The `Documentation` workflow in `.github/workflows/docs.yml` builds the site with strict validation. Pull requests run the build; documentation changes pushed to `main` also deploy the generated `site/` artifact. The workflow can be run manually from the Actions tab.

In the repository's **Settings → Pages → Build and deployment**, choose **GitHub Actions** as the source. MkDocs configuration describes the site; the Actions workflow builds and publishes it. See [GitHub's custom Pages workflow guide](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).

The project site is [nnamllit1.github.io/Control-Resonant-Mod-Loader](https://nnamllit1.github.io/Control-Resonant-Mod-Loader/). If its published address changes, update `site_url` in `mkdocs.yml`.

Public pages address players, mod authors, and contributors. Local binary-analysis reports belong in the ignored `.local/` directory. Do not publish game binaries or personal installation details as project assets.

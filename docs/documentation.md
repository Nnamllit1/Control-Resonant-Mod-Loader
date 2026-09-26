---
description: Build and publish CRML documentation with MkDocs, maintain canonical URLs and the sitemap, and verify crawler access for the public site.
---

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

The public site is [crml.nnamllit.de](https://crml.nnamllit.de/). Keep `site_url` in `mkdocs.yml`, `docs/CNAME`, and the sitemap URL in `docs/robots.txt` aligned with that domain. The Pages custom domain must also be configured in repository settings; a CNAME file alone does not configure a custom Actions deployment.

## Search discovery and crawler access

MkDocs generates `sitemap.xml` and `sitemap.xml.gz` on every build. The sitemap lists canonical documentation pages on the public domain; raw research JSON and the 404 page are not sitemap entries. The template in `overrides/sitemap.xml` omits modification dates because build dates do not describe when each page's content last changed.

`docs/robots.txt` permits crawling and advertises `https://crml.nnamllit.de/sitemap.xml`. Page front matter supplies descriptive titles and unique summaries. `overrides/main.html` adds Open Graph and Twitter metadata, homepage `WebSite` structured data, and a `noindex` directive for the error page. The generated system index gets its front matter from `tools/engine_atlas.py`; keep generated content and its generator consistent.

After deploying:

1. Open the homepage, `robots.txt`, and `sitemap.xml` on the public domain. Each should return its expected content, without a login or crawler challenge.
2. Inspect the homepage and a research page's source: their canonical URLs must use `https://crml.nnamllit.de/`, not the former GitHub Pages URL.
3. In Google Search Console, select the verified property for this domain and submit `https://crml.nnamllit.de/sitemap.xml` under **Sitemaps**.
4. Use **URL Inspection** on the homepage and the engine research page to check Google's live access and request indexing.

A sitemap helps discovery; it does not guarantee indexing or search position. See Google's [sitemap submission guide](https://developers.google.com/search/docs/crawling-indexing/sitemaps/build-sitemap) and [site-name structured data guide](https://developers.google.com/search/docs/appearance/site-names). Public research pages should describe actual findings and their limits, using clear subsystem names rather than repeated search keywords.

Public pages address players, mod authors, and contributors. Local binary-analysis reports, individual play-session results, and private progress notes belong in the ignored `.local/` directory. Do not publish game binaries, local drive names, user or machine names, installation paths, or save-specific details as project assets. Command examples should prompt for a reader's location or use repository-relative paths.

Run `python tools/check_public_paths.py` before publishing. The documentation workflow checks repository text and generated pages for absolute drive paths, network shares and common user/mounted-volume directories. It reports locations without echoing the matched text. This catches accidental path publication; it does not replace reviewing content for other personal information.

# nordstjernen.org

The source of [nordstjernen.org](https://nordstjernen.org), the website of
the Nordstjernen project and its browsers: Nordstjernen, Northstar and
Southstar, and Weststar, which has no release yet.

The site is plain static HTML with one stylesheet, `assets/style.css`, and no
build step. GitHub Pages publishes this `website` branch as it is, at the
domain in `CNAME`.

## Layout

| Path | What it is |
|------|------------|
| `index.html` | the English home page |
| `index_xx.html` | the home page in 20 other languages |
| `news.html`, `specs.html`, `download.html`, `screenshots.html` | the full news list, the comparison table, all downloads, the screenshot gallery |
| `northstar-browser/` | the Northstar page |
| `release-*.html`, `northstar-release-*.html` | release announcements |
| `screenshots/`, `assets/` | images, the logo and the stylesheet |
| `privacy/` | the privacy policy |
| `sitemap.xml`, `robots.txt` | for search engines |

## Previewing

Open `index.html` in a browser, or serve the directory, e.g.
`python3 -m http.server`, and visit <http://localhost:8000/>.

## Changing the site

Open a pull request against the `website` branch; see
[CONTRIBUTING.md](CONTRIBUTING.md). When a release ships, update the version,
release lines, news, comparison table and download links on `index.html`,
`download.html` and, for Northstar, `northstar-browser/index.html`; add the
item to `news.html` too, and a new page to `sitemap.xml`.

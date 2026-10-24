# Quick Start

Welcome to **Muffin**, a fast, native Markdown editor built with C++ and Qt.

## Create or open a document

- Choose **File → New** for a blank document, or **File → Open** to open an existing `.md` file.
- **File → Open Folder** opens a folder in the sidebar so you can browse and switch files.

## Two editing modes

- **Render mode** (the default) shows a live, formatted page — type Markdown, see the result.
- **Source mode** shows the raw Markdown text. Toggle it from the **View** menu.

## Write your first document

Type Markdown and it is formatted instantly. A few essentials:

| You type | You get |
| --- | --- |
| `# Heading` | A level‑1 heading |
| `**bold**` / `*italic*` | **bold** / *italic* |
| `- item` | A bullet list |
| `` `code` `` | Inline `code` |
| `[link](https://example.com)` | A clickable link |

Insert tables, math (`$E=mc^2$`), footnotes, and fenced code blocks — Muffin renders all of GitHub‑Flavored Markdown.

## Tips

- In live editing, paste browser or office HTML to keep headings, emphasis, lists, links and tables. Spreadsheet cells copied as tab-separated text become a Markdown table. Use **Edit → Paste as Plain Text** to keep only text; code blocks and source mode always paste literal text.
- Copying in live editing provides HTML for rich editors and Markdown for Muffin. **Preferences → Editor** controls the plain-text representation sent to plain-text apps.
- Use **Preferences → Keyboard Shortcuts** to search commands, record a key combination, resolve conflicts or restore defaults. Changes apply immediately and are saved for future sessions.
- In **Preferences → Export → PDF**, configure the header, footer, author and page numbers. Templates support `${title}`, `${author}`, `${fileName}`, `${date}`, `${pageNo}` and `${pageCount}`.
- Press the formatting and paragraph entries in the menus to style text without remembering syntax.
- **Edit → Find / Replace** searches the current document.
- Pick a look under **Theme** — Muffin ships light, dark and "Whitey" themes.

For the full syntax reference, see the [Markdown Reference](help:markdown-reference).

To see who makes Muffin possible, read the [Acknowledgements](help:acknowledgements).

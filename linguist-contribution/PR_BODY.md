## Description
Add **Kairos**, a reversible and concurrent programming language (Janus-inspired):
every program can be executed both forward and backward, and the inverse of any
computation is always well-defined. Source files use the `.kairos` extension.
Toolchain + reversible VM: https://github.com/nicologiuliani6/kairos

## Checklist:
- [x] **I am adding a new language.**
  - [ ] The extension of the new language is used in hundreds of repositories on GitHub.com.
    - Search results for each extension:
      - https://github.com/search?type=code&q=NOT+is%3Afork+path%3A*.kairos+procedure+delocal
  - [x] I have included a real-world usage sample for all extensions added in this PR:
    - Sample source(s):
      - https://github.com/nicologiuliani6/kairos/tree/main/examples
    - Sample license(s): MIT
  - [x] I have included a syntax highlighting grammar:
      - https://github.com/nicologiuliani6/kairos-vscode-debugger (scope `source.kairos`, MIT)
  - [x] I have added a color
    - Hex value: `#8A2BE2`
    - Rationale: blueviolet, chosen to evoke the "time / reversible" theme of the
      language and to stay visually distinct from neighbouring languages in the bar.
  - [ ] I have updated the heuristics to distinguish my language from others using the same extension.
    - Not needed: `.kairos` is a unique extension, no collision with existing languages.

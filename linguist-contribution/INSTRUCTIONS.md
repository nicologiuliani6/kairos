# Registering "Kairos" on GitHub (github-linguist)

GitHub recognizes languages through [github-linguist](https://github.com/github-linguist/linguist).
To show **Kairos** in the language bar of *every* repository (kairos, mnemo, …)
an upstream PR is needed. Once merged, the effect is global and automatic.

## Prerequisites
- **Grammar published with a LICENSE.** The TextMate grammar lives in
  `github.com/nicologiuliani6/kairos-vscode-debugger`
  (`syntaxes/kairos.tmLanguage.json`, `scopeName: source.kairos`).
  `script/add-grammar` requires that repository to have a `LICENSE` file
  (added: MIT). Make sure the latest grammar (with `try/rollback/yrt`) and the
  LICENSE are **pushed**.
- **Samples** in `samples/Kairos/` of this folder (at least ~200 bytes required).

## How the "fork" works (in short)
You cannot write to GitHub's repository (`github-linguist/linguist`).
The standard flow is:
1. **Fork** = your own copy of the repository under your account
   (`nicologiuliani6/linguist`). Click "Fork" on the website.
2. Clone **your** fork, make the changes on a branch, push them to your fork.
3. Open a **Pull Request** from your fork to the original repository: the
   maintainers review it and, if accepted, merge it.

## Steps

1. **Fork**: open `https://github.com/github-linguist/linguist`, click **Fork**
   (top right). It creates `https://github.com/nicologiuliani6/linguist`.

2. **Clone your fork and create a branch**
   ```bash
   git clone --recursive git@github.com:nicologiuliani6/linguist.git
   cd linguist
   git checkout -b add-kairos
   bundle install
   ```

3. **Add the grammar** (it creates the submodule and updates `grammars.yml`):
   ```bash
   script/add-grammar https://github.com/nicologiuliani6/kairos-vscode-debugger
   ```

4. **languages.yml**: in `lib/linguist/languages.yml`, in alphabetical order
   (the K section), insert:
   ```yaml
   Kairos:
     type: programming
     color: "#8A2BE2"
     extensions:
     - ".kairos"
     tm_scope: source.kairos
     ace_mode: text
     language_id: 790447070
   ```

5. **Samples**
   ```bash
   mkdir -p samples/Kairos
   cp ~/Desktop/kairos/linguist-contribution/samples/Kairos/*.kairos samples/Kairos/
   ```

6. **Unique language_id and tests**
   ```bash
   script/update-ids                 # validates/assigns language_id (changes it on a clash)
   bundle exec rake test             # must pass
   bin/github-linguist --breakdown   # optional: check that .kairos -> Kairos
   ```

7. **Commit, push to your fork, PR**
   ```bash
   git add -A
   git commit -m "Add Kairos language"
   git push origin add-kairos
   ```
   GitHub then shows the button to open the **Pull Request** towards
   `github-linguist/linguist`. Fill in the template (tick the checkboxes).

## ⚠️ Reality
- The PR template asks that the language be **used in several public
  repositories**. `kairos` and `mnemo` use `.kairos` for real, but the
  maintainers may ask for wider use: acceptance is **not guaranteed**, and the
  review is **slow** (weeks to months).
- After the merge, a Linguist release and GitHub's deploy, nothing is needed
  per repository: `.kairos` is counted as **Kairos** everywhere. At that point
  the mapping to Pascal can be removed from the `.gitattributes`.

## Alternative already active (meanwhile)
The `.gitattributes` map `.kairos` to **Pascal**, so they show up in the
language bar right away (labelled Pascal, not "Kairos"). They stay until the
upstream merge.

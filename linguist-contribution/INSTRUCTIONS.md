# Registrare "Kairos" su GitHub (github-linguist)

GitHub riconosce le lingue tramite [github-linguist](https://github.com/github-linguist/linguist).
Per far comparire **Kairos** nella barra lingue di *tutte* le repo (kairos, mnemo, …)
serve una PR upstream. Una volta merge-ata, l'effetto è globale e automatico.

## Prerequisiti
- **Grammar pubblicato con LICENSE.** Il grammar TextMate sta in
  `github.com/nicologiuliani6/kairos-vscode-debugger`
  (`syntaxes/kairos.tmLanguage.json`, `scopeName: source.kairos`).
  `script/add-grammar` richiede che quel repo abbia un file `LICENSE`
  (aggiunto: MIT). Assicurati che l'ultima versione del grammar — con
  `try/rollback/yrt` — e il LICENSE siano **pushati**.
- **Samples** in `samples/Kairos/` di questa cartella (≥ ~200 byte richiesti).

## Come funziona il "fork" (in breve)
Non hai i permessi di scrittura sul repo di GitHub (`github-linguist/linguist`).
Il flusso standard è:
1. **Fork** = una tua copia del repo sotto il tuo account
   (`nicologiuliani6/linguist`). Clicchi "Fork" sul sito.
2. Cloni il **tuo** fork, fai le modifiche su un branch, le pushi sul tuo fork.
3. Apri una **Pull Request** dal tuo fork verso il repo originale: i maintainer
   la revisionano e (se accettata) la fanno merge nel loro repo.

## Passi

1. **Fork** — apri `https://github.com/github-linguist/linguist`, click **Fork**
   (in alto a destra). Crea `https://github.com/nicologiuliani6/linguist`.

2. **Clone del tuo fork + branch**
   ```bash
   git clone --recursive git@github.com:nicologiuliani6/linguist.git
   cd linguist
   git checkout -b add-kairos
   bundle install
   ```

3. **Aggiungi il grammar** (crea il submodule e aggiorna `grammars.yml` da solo):
   ```bash
   script/add-grammar https://github.com/nicologiuliani6/kairos-vscode-debugger
   ```

4. **languages.yml** — in `lib/linguist/languages.yml`, in ordine alfabetico
   (zona K), inserisci:
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

6. **language_id univoco + test**
   ```bash
   script/update-ids                 # valida/assegna language_id (cambia se collide)
   bundle exec rake test             # deve passare
   bin/github-linguist --breakdown   # opzionale: verifica .kairos -> Kairos
   ```

7. **Commit, push sul tuo fork, PR**
   ```bash
   git add -A
   git commit -m "Add Kairos language"
   git push origin add-kairos
   ```
   Su GitHub apparirà il bottone per aprire la **Pull Request** verso
   `github-linguist/linguist`. Compila il template (spunta le checkbox).

## ⚠️ Realtà
- Il template della PR chiede che la lingua sia **in uso in più repository
  pubblici**. Hai `kairos` + `mnemo` con `.kairos` (uso reale), ma i maintainer
  possono chiedere più diffusione: accettazione **non garantita**, review
  **lenta** (settimane–mesi).
- Dopo merge + release Linguist + deploy GitHub: nessuna azione per-repo,
  `.kairos` viene conteggiato come **Kairos** ovunque. A quel punto si può
  rimuovere il mapping a Pascal dai `.gitattributes`.

## Alternativa già attiva (nel frattempo)
I `.gitattributes` mappano i `.kairos` a **Pascal** così appaiono subito nella
barra lingue (etichetta non "Kairos"). Restano fino al merge upstream.

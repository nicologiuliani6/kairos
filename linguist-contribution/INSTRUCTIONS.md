# Registrare "Kairos" su GitHub (github-linguist)

GitHub riconosce le lingue tramite [github-linguist](https://github.com/github-linguist/linguist).
Per far comparire **Kairos** nella barra lingue di *tutte* le repo (kairos, mnemo, …)
serve una PR upstream. Una volta merge-ata, l'effetto è globale e automatico.

## Prerequisiti
- Il grammar TextMate è già pubblicato: `github.com/nicologiuliani6/kairos-vscode-debugger`
  → `syntaxes/kairos.tmLanguage.json`, `scopeName: source.kairos`.
  Assicurati che l'ultima versione del grammar (con `try/rollback/yrt`) sia
  **committata e pushata** su quel repo prima di aprire la PR.
- Linguist richiede ≥ ~200 byte di campioni per lingua: i file in
  `samples/Kairos/` di questa cartella soddisfano il requisito.

## Passi

1. **Fork & clone**
   ```bash
   git clone --recursive git@github.com:<tuo-user>/linguist.git
   cd linguist
   git checkout -b add-kairos
   ```

2. **languages.yml** — inserisci la voce di `languages.yml.snippet` in
   `lib/linguist/languages.yml` (ordine alfabetico).

3. **Grammar come submodule**
   ```bash
   git submodule add https://github.com/nicologiuliani6/kairos-vscode-debugger \
     vendor/grammars/kairos-vscode-debugger
   ```
   Poi aggiungi la voce di `grammars.yml.snippet` a `grammars.yml`
   (oppure rigenera con `script/convert-grammars`).

4. **Samples** — copia i file:
   ```bash
   mkdir -p samples/Kairos
   cp <questa-cartella>/samples/Kairos/*.kairos samples/Kairos/
   ```

5. **language_id univoco**
   ```bash
   bundle install
   script/update-ids        # assegna/valida language_id
   ```

6. **Test**
   ```bash
   bundle exec rake test
   bin/github-linguist --breakdown   # verifica che i .kairos siano rilevati come Kairos
   ```

7. **Commit & PR**
   ```bash
   git add -A && git commit -m "Add Kairos language"
   git push origin add-kairos
   ```
   Apri la PR verso `github-linguist/linguist`. Nota: GitHub accetta nuove
   lingue con uso reale dimostrabile (repo pubblici con `.kairos`).

## Dopo il merge
Nessuna azione per-repo: GitHub ricalcola le lingue al prossimo push/refresh,
e `.kairos` viene conteggiato come **Kairos** sia su `kairos` sia su `mnemo`.

## Alternativa rapida (nel frattempo)
Se vuoi colorare/contare i `.kairos` SUBITO senza attendere l'upstream, si può
aggiungere un `.gitattributes` per repo con override `linguist-language` verso
una lingua già nota a Linguist (etichetta non "Kairos"). Chiedi se la vuoi.

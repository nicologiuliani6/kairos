# Kairos VS Code Extension

Estensione VS Code per il linguaggio `Kairos` con:

- Debugger (DAP) `kairos`
- Evidenziazione sintassi base (`.kairos`)

## Funzionalita

- Supporto file con estensione `.kairos`
- Breakpoint su sorgenti Kairos
- Configurazione debug `kairos` in `launch.json`
- Highlight parole chiave principali del linguaggio:
  - controllo: `if`, `then`, `else`, `fi`, `from`, `loop`, `until`
  - chiamate/scope: `call`, `uncall`, `local`, `delocal`
  - concorrenza: `par`, `and`, `rap`
  - tipi/costanti: `procedure`, `int`, `stack`, `channel`, `nil`, `empty`

## Struttura

- `src/extension.ts`: entrypoint estensione
- `src/dapAdapter.ts`: adapter/debug runtime bridge
- `syntaxes/kairos.tmLanguage.json`: regole di syntax highlighting
- `package.json`: contributi VS Code (language/debugger/grammar/comandi)

## Sviluppo locale

Dentro `vscode-extension`:

```bash
npm install
make compile
make package
make install
```

Comandi utili:

- `make watch`: build TypeScript in watch mode
- `make reinstall`: disinstalla + reinstalla l'estensione
- `make uninstall`: rimuove l'estensione da VS Code
- `make clean`: pulizia artefatti (`out`, `*.vsix`)

## Uso rapido

1. Apri un file `.kairos`: vedrai il colore sulle keyword.
2. Nella cartella dove lavori con i file `.kairos`, crea (o modifica) `./.vscode/launch.json`.
3. Avvia il debug da pannello Run and Debug.

## Configurazione `launch.json` (importante)

Il file deve stare nella workspace che contiene i tuoi sorgenti Kairos, quindi tipicamente:

- `progetto/.vscode/launch.json`

Esempio pratico (uguale al tuo setup in `examples/.vscode/launch.json`):

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "Kairos: Debug file corrente",
      "type": "kairos",
      "request": "launch",
      "program": "${file}",
      "kairosApp": "/home/nico/Desktop/kairos/build/dist/KairosApp",
      "kairosLib": "/home/nico/Desktop/kairos/build/libvm_dap.so"
    }
  ]
}
```

Significato dei campi chiave:

- `program`: file `.kairos` da eseguire (es. `${file}`)
- `kairosApp`: percorso dell'eseguibile compilato (`build/dist/KairosApp`)
- `kairosLib`: percorso della libreria DAP (`build/libvm_dap.so`)

Se `kairosApp` o `kairosLib` non sono corretti, il debug non parte.

## Note

- L'highlighting attuale e volutamente minimale (keyword/operatori/commenti/numeri).
- Se servono regole aggiuntive (funzioni, variabili, scope piu fini), estendere `syntaxes/kairos.tmLanguage.json`.

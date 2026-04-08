import * as vscode from 'vscode';
import * as path from 'path';

export function activate(context: vscode.ExtensionContext) {
    // aggiungi queste due righe all'inizio
    //const adapterPath = path.join(__dirname, 'dapAdapter.js');
    //vscode.window.showInformationMessage(`Janus: adapterPath=${adapterPath}`);

    // ── Comando: seleziona interprete Python ────────────────────────────
    const selectInterpreter = vscode.commands.registerCommand(
        'janus.selectInterpreter',
        async () => {
            const current = vscode.workspace
                .getConfiguration('janus')
                .get<string>('pythonPath', '');

            const choice = await vscode.window.showQuickPick(
                [
                    { label: '$(folder) Scegli file...', id: 'browse' },
                    { label: '$(close) Usa default (venv automatico)', id: 'default' },
                ],
                { placeHolder: `Interprete attuale: ${current || 'default'}` }
            );

            if (!choice) return;

            if (choice.id === 'default') {
                await vscode.workspace
                    .getConfiguration('janus')
                    .update('pythonPath', '', vscode.ConfigurationTarget.Global);
                vscode.window.showInformationMessage('Janus: interprete reimpostato al default.');
                return;
            }

            const uris = await vscode.window.showOpenDialog({
                canSelectFiles:    true,
                canSelectFolders:  false,
                canSelectMany:     false,
                openLabel:         'Seleziona interprete Python',
                filters: { 'Eseguibili': ['', 'exe'], 'Tutti i file': ['*'] },
            });

            if (!uris || uris.length === 0) return;

            const chosen = uris[0].fsPath;
            await vscode.workspace
                .getConfiguration('janus')
                .update('pythonPath', chosen, vscode.ConfigurationTarget.Global);

            vscode.window.showInformationMessage(`Janus: interprete impostato su ${chosen}`);
        }
    );

    // ── Status bar ───────────────────────────────────────────────────────
    const statusBar = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Left, 100
    );
    statusBar.command = 'janus.selectInterpreter';
    statusBar.tooltip = 'Clicca per cambiare interprete Python per Janus';

    function updateStatusBar() {
        const p = vscode.workspace
            .getConfiguration('janus')
            .get<string>('pythonPath', '');
        statusBar.text = `$(python) Janus: ${p ? path.basename(p) : 'default'}`;
        statusBar.show();
    }

    updateStatusBar();
    vscode.workspace.onDidChangeConfiguration(e => {
        if (e.affectsConfiguration('janus.pythonPath')) updateStatusBar();
    });

    // ── Factory DAP ──────────────────────────────────────────────────────
    const factory = new JanusDebugAdapterDescriptorFactory();
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('janus', factory),
        selectInterpreter,
        statusBar,
    );
}

export function deactivate() {}

class JanusDebugAdapterDescriptorFactory
    implements vscode.DebugAdapterDescriptorFactory
{
    createDebugAdapterDescriptor(
        _session: vscode.DebugSession,
        _executable: vscode.DebugAdapterExecutable | undefined
    ): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
        const adapterPath = path.join(__dirname, 'dapAdapter.js');

        const config     = vscode.workspace.getConfiguration('janus');
        const pythonPath = config.get<string>('pythonPath', '');
        const libPath    = config.get<string>('libPath', '');      // ← nuovo

        const env: { [key: string]: string } = {};
        for (const [k, v] of Object.entries(process.env)) {
            if (v !== undefined) env[k] = v;
        }
        if (pythonPath) env['JANUS_PYTHON_PATH'] = pythonPath;
        if (libPath)    env['JANUS_LIB_PATH']    = libPath;        // ← nuovo

        return new vscode.DebugAdapterExecutable('node', [adapterPath], { env });
    }
}
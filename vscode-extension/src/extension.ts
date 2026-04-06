import * as vscode from 'vscode';
import * as path from 'path';

export function activate(context: vscode.ExtensionContext) {
    // Factory che crea il DAP adapter come processo separato
    const factory = new JanusDebugAdapterDescriptorFactory();
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('janus', factory)
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
        // Il DAP adapter gira come processo Node.js separato
        const adapterPath = path.join(__dirname, 'dapAdapter.js');
        return new vscode.DebugAdapterExecutable('node', [adapterPath]);
    }
}
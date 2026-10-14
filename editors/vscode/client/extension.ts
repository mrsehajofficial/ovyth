// Ovyth :: editors/vscode/client/extension.ts
// The client half: register the language (done by package.json's
// contributions) and hand the server -- built by `make lsp` into
// bin/ovyth-lsp -- to vscode-languageclient. The server binary must exist
// on this machine: `make` builds it next to the compiler.
import { commands, ExtensionContext, workspace } from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';
import * as fs from 'fs';
import * as path from 'path';

// The server the .vsix ships (bin/ovyth-lsp, built by `make lsp`), found
// before anything on PATH, so an installed extension does not depend on the
// host having `make lsp` output on its PATH. The ovyth.serverPath setting
// overrides when set.
function resolveServer(context: ExtensionContext): string {
  const override = workspace.getConfiguration('ovyth').get<string>('serverPath');
  if (override) return override;
  const vendored = path.join(context.extensionPath, 'bin', 'ovyth-lsp');
  if (fs.existsSync(vendored)) return vendored;
  return 'ovyth-lsp';
}

export async function activate(context: ExtensionContext): Promise<void> {
  const serverOptions: ServerOptions = {
    command: resolveServer(context),
    transport: TransportKind.stdio,
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'ovyth' }],
    synchronize: {
      fileEvents: [workspace.createFileSystemWatcher('**/*.ov')],
    },
  };

  const client = new LanguageClient(
    'ovyth',
    'Ovyth Language Server',
    serverOptions,
    clientOptions
  );

  client.start().then(
    () => undefined,
    (err) => console.error(`ovyth: language server failed to start: ${err}`)
  );

  context.subscriptions.push(
    commands.registerCommand('ovyth.restartServer', async () => {
      await client.stop();
      await client.start();
    })
  );
}

export function deactivate(): Thenable<void> | undefined {
  // The client is stopped by the subscription that started it.
  return undefined;
}

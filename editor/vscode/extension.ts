import * as vscode from 'vscode';
import { execFile } from 'child_process';
import { LanguageClient, LanguageClientOptions,
         ServerOptions, Trace } from 'vscode-languageclient/node';

let client: LanguageClient;

// saqut binary'sinin yolunu saqut.path ayarından çözer (varsayılan: PATH'teki 'saqut').
function saqutBinary(): string {
    const cfg = vscode.workspace.getConfiguration('saqut');
    return cfg.get<string>('path', 'saqut');
}

// Binary'nin gerçek sürümünü alır. LSP sunucusu serverInfo.version'da bu değeri
// bildirir (SAQUT_VERSION); buradaki kontrol kullanıcıya uyumsuzlukta erken uyarı verir.
function getSaqutVersion(bin: string): Promise<string | null> {
    return new Promise((resolve) => {
        execFile(bin, ['--version'], { timeout: 5000 },
            (err, stdout) => {
                if (err) { resolve(null); return; }
                const m = /saQut\s+(\d+\.\d+\.\d+)/.exec(stdout);
                resolve(m ? m[1] : null);
            });
    });
}

// LSP/DAP için gereken minimum derleyici sürümü: 1.0.0 (LSP/DAP düzeltmeleri).
function versionAtLeast(ver: string | null): boolean {
    if (!ver) return false;
    const [major, minor] = ver.split('.').map(Number);
    return major >= 1;
}

async function checkSaqutVersion(bin: string): Promise<void> {
    const ver = await getSaqutVersion(bin);
    if (ver === null) {
        vscode.window.showErrorMessage(
            `saQut: '${bin}' bulunamadı veya --version çalışmıyor. ` +
            `Derleyici PATH'te değilse 'saqut.path' ayarını mutlak yola ayarlayın.`);
        return;
    }
    if (!versionAtLeast(ver)) {
        vscode.window.showWarningMessage(
            `saQut: bulunan derleyici ${ver} — bu uzantı 1.0.0+ gerektirir. ` +
            `'saqut.path' ile başka bir binary seçin.`);
    }
}

export function activate(ctx: vscode.ExtensionContext) {
    const bin = saqutBinary();

    const serverOptions: ServerOptions = {
        command: bin,
        args: ['lsp']
    };
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'sqt' }],
        outputChannelName: 'saQut Language Server',
        traceOutputChannel: vscode.window.createOutputChannel('saQut LSP Trace')
    };
    client = new LanguageClient('saQut', 'saQut Language Server',
                                 serverOptions, clientOptions);

    // Trace seviyesi ayara bağlı — 'verbose' açılmadıkça LSP paketleri yazılmaz.
    const trace = vscode.workspace.getConfiguration('saqut').get<string>('trace.server', 'off');
    client.start().then(() => {
        if (trace === 'verbose') client.setTrace(Trace.Verbose);
    });
    ctx.subscriptions.push(client);

    // saqut.path değişince restart komutu ile yeni binary devreye girer.
    ctx.subscriptions.push(
        vscode.commands.registerCommand('saqut.restartLanguageServer', async () => {
            await client.restart();
            vscode.window.showInformationMessage('saQut: Language Server yeniden başlatıldı.');
        })
    );

    const debugFactory: vscode.DebugAdapterDescriptorFactory = {
        createDebugAdapterDescriptor(_session) {
            // JIT (--jit) tam-program derlemesi yapar, breakpoint/adım desteklemez —
            // DAP yalnızca VM ile çalışır; bu yüzden buraya JIT bayrağı geçirilmez.
            return new vscode.DebugAdapterExecutable(bin, ['dap']);
        }
    };
    ctx.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('sqt', debugFactory)
    );

    // Sürüm kontrolü aktivasyonu bloklamaz; uyumsuzluk bildirimi arka planda gelir.
    checkSaqutVersion(bin);
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}

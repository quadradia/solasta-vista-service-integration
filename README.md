# solasta-vista-service-integration

`vista-sync.exe` is a small native Windows service. It reads the Vista ERP item master (`IPART.dbf`, Visual FoxPro) on the Cimtech XP machine and sends new and changed items straight to the Solasta API over HTTPS.

Target machine: **Windows XP Pro SP3, 32-bit, Pentium II 363 MHz (no SSE/SSE2), 256 MB RAM**. So the service is:
- native C++ (no .NET);
- built with the `v141_xp` toolset, `/arch:IA32`, the static CRT, `_WIN32_WINNT=0x0501` and PE subsystem 5.01;
- using its own TLS 1.2 stack, because XP's SChannel stops at TLS 1.0.

## What one sync does

1. Copies `IPART.dbf` from `[Vista] DataDir` to `WorkDir\snapshot\` and reads only the copy, so Vista's own file is never held open.
2. Reads the DBF header and fields, skips records marked deleted (`*`), and decodes text from Windows-1252.
3. Maps each live part:

   | Vista | JSON | Rule |
   |---|---|---|
   | `IPPART` | `externalId` | trimmed, as in Vista |
   | `IPPART` | `sku` | upper-cased (the API stores SKUs upper-case) |
   | `IPDESC` | `name` | the SKU if blank |
   | `IPCLAS` | `category` | `null` if blank |
   | `IPIUM` | `unitOfMeasure` | `EA` → `each`, `FT` → `ft`; any other unit skips the part |
   | `IPSMUC` / `IPAMUC` | `standardCost` | standard cost when > 0, otherwise average cost; rounded half away from zero to 4 decimals |
   | `IPUPRC` | `listPrice` | 4 decimals; sent even if the API ignores it for now |

   Parts in class `TEST` or with a SKU starting `SHIP-` are skipped (`SkipClasses`, `SkipSkuPrefixes`).
4. Hashes each item's JSON (SHA-256) and compares it with `WorkDir\item-state.txt`. Only new or changed items are sent.
5. Gets an OAuth2 `client_credentials` token, then POSTs the changed items in **one request per run**, to limit TLS handshakes on the slow CPU (`MaxItemsPerRequest` only splits very large batches):

   ```json
   {"source":"VISTA","company":"SO","items":[{"externalId":"MTSO-0001","sku":"MTSO-0001","name":"LIGHTS UCS2904 RGBW(600k) 1M","category":"LIGHT","unitOfMeasure":"ft","standardCost":8.8,"listPrice":10.12}]}
   ```
6. On HTTP 2xx, records the new hashes. On any failure nothing is recorded, so the next run sends the same items again. On a 401, it gets a fresh token and retries once.

Not in v1: on-hand quantities (`IPQOH`), deactivating parts that disappear from Vista (only logged as a warning), and per-row results from the API. The import endpoint does not exist yet in the Solasta API (see `Documentation/vista-erp-import-answers.md` §1.3), so `ImportUrl` is the planned route.

## Build

You need **Visual Studio 2019 Build Tools** with the *C++ build tools* workload, plus the component **"C++ Windows XP Support for VS 2017 (v141) tools"** (`Microsoft.VisualStudio.Component.WinXP`). That component brings the `v141_xp` toolset and the Windows 7.1A SDK. The CMake bundled with the Build Tools is enough.

```bat
build.cmd            :: v141_xp, the build for the XP machine -> build\v141_xp\Release\vista-sync.exe
build.cmd v142       :: dev build on a PC without the XP component (Microsoft doesn't support it on XP)
```

With the Build Tools 2019 setup on the dev PC, the `v141_xp` toolset compiles with MSVC 14.29 against the Windows 7.1A SDK and the XP-compatible static UCRT (10.0.10240), with `_USING_V110_SDK71_`. To use the original VS 2017 compiler (14.16) instead, also add the component `Microsoft.VisualStudio.Component.VC.v141.x86.x64`.

`tools\export-ca-bundle.ps1 -OutFile <path>\ca-bundle.pem` builds a starter CA bundle of common public root CAs from the dev PC's Windows root store. Trim it once the real certificate chains of the token and API hosts are confirmed.

The build compiles the vendored mbedTLS (`third_party/mbedtls`, 3.6.8 LTS, unmodified) as a static library. It also compiles the Event Log message table (`src/messages.mc`) with `mc.exe`.

Check every build that goes to the XP machine:

```bat
python -I tests\check_xp_pe.py build\v141_xp\Release\vista-sync.exe
```

It checks that the exe is 32-bit x86 with OS/subsystem version 5.01. It also checks that every DLL and function the exe imports is on a list verified against XP SP3. A new import fails the check until someone verifies it and adds it to the list.

## Install on the XP machine

1. Make a folder, for example `C:\SolastaVistaSync`, and copy in `vista-sync.exe`.
2. Copy `config\vista-sync.example.ini` to `vista-sync.ini` and set `DataDir`, the URLs and the client id.
3. Put the PEM root CA certificate(s) for the token host and the API host in `ca-bundle.pem`. XP's certificate store is not used, and its roots are years out of date.
4. Store the client secret. Put it in a temporary text file, then run:

   ```bat
   vista-sync.exe --protect-secret secret.txt client-secret.bin
   del secret.txt
   ```

   This encrypts it with machine-scope DPAPI. Only this machine can decrypt it, so run it there, not on the build PC. Set `ClientSecretDpapiFile=client-secret.bin`. A plain `ClientSecretFile` also works, but then lock that file down to Administrators and SYSTEM.
5. Test once in a console:

   ```bat
   vista-sync.exe --run-once --dry-run     :: prints the JSON, calls nothing, changes nothing
   vista-sync.exe --run-once               :: real sync
   ```

6. Install and start the service, as Administrator:

   ```bat
   vista-sync.exe --install
   sc start SolastaVistaSync
   ```

   `--install` creates the automatic-start service `SolastaVistaSync`, which depends on Tcpip and restarts on failure. It also registers the Event Log source.

   The equivalent with `sc` alone is below. Note the space after each `=`:

   ```bat
   sc create SolastaVistaSync binPath= "\"C:\SolastaVistaSync\vista-sync.exe\" --service --config \"C:\SolastaVistaSync\vista-sync.ini\"" start= auto depend= Tcpip DisplayName= "Solasta Vista Item Sync"
   ```

   The service runs as LocalSystem. If the Vista tables are on a network share, run it as a domain or local account that can read the share: `sc config SolastaVistaSync obj= DOMAIN\user password= ...`.
7. Remove it with `vista-sync.exe --uninstall`.

Console output: progress and success messages go to stdout, and only warnings and errors go to stderr, so Windows PowerShell shows only real problems in red (as `NativeCommandError`). The exit code is 0 on success and 1 on failure. With `--dry-run > dryrun.json` the file gets only the JSON body, and the progress lines go to stderr; in a plain console window they all show together.

Logs go to `logs\vista-sync-YYYY-MM-DD.log`; old files are deleted after `RetentionDays`. Warnings, errors, service start/stop and sync summaries also go to the Application event log, source `SolastaVistaSync`.

## Configuration (`vista-sync.ini`)

See `config/vista-sync.example.ini`. Relative paths are relative to the exe's folder.

| Section | Key | Default | |
|---|---|---|---|
| Vista | `DataDir` | (required) | folder with `IPART.dbf` of company SO |
| Vista | `ItemTable`, `Source`, `Company` | `IPART.dbf`, `VISTA`, `SO` | |
| Vista | `SkipClasses`, `SkipSkuPrefixes` | `TEST`, `SHIP-` | comma-separated |
| Sync | `IntervalMinutes`, `StartupDelaySeconds` | 60, 60 | |
| Sync | `MaxItemsPerRequest`, `WorkDir` | 500, `work` | |
| Api | `TokenUrl`, `ClientId`, `Scope` | | OAuth2 client credentials |
| Api | `TokenAuth` | `post` | `post` = secret in the form body, `basic` = HTTP Basic |
| Api | `ClientSecretDpapiFile` / `ClientSecretFile` | | the secret is never in the INI or the code |
| Api | `ImportUrl`, `CaFile`, `TimeoutSeconds` | `ca-bundle.pem`, 120 | |
| Log | `Dir`, `RetentionDays`, `EventLog` | `logs`, 30, 1 | |

## TLS approach

XP's SChannel/WinHTTP/WinINet can't do TLS 1.2, and `api.mysolasta.com` and the identity server require it. So the service has its own TLS stack: **mbedTLS 3.6 LTS**, compiled into the exe, over plain Winsock. The options are in `src/mbedtls_user_config.h`:
- TLS 1.2 only (min = max), with the server certificate and host name always verified against `CaFile`.
- Entropy from `CryptGenRandom` (advapi32, available on XP) instead of mbedTLS's default `BCryptGenRandom`, which needs Vista.
- No `net_sockets.c`, and mbedTLS's software `inet_pton`, because `inet_pton` needs Vista.
- AES-NI/PCLMUL code stays in, but mbedTLS only uses it after CPUID reports support, which a Pentium II won't. The CRT's SSE2 `memcpy`/string/math variants are also chosen at run time. The application and mbedTLS are compiled with `/arch:IA32`.

The XP machine pays for one or two TLS handshakes per run (token + import, each RSA/ECDHE). Expect a few seconds each at 363 MHz.

## Tests

- `python -I tests\check_dryrun.py <IPART.dbf> <dryrun.json>`: reads the DBF independently in Python and checks every field of the `--dry-run` output.
- `python -I tests\mock_api_server.py --cert cert.pem --key key.pem --secret-file secret.txt --out received.jsonl`: a local TLS-1.2-only HTTPS stand-in for `/connect/token` and `/api/inventory/import/items`, with a self-signed certificate. Point a test INI's `TokenUrl`, `ImportUrl` and `CaFile` at it. It never talks to the real services.
- `python -I tests\check_xp_pe.py <exe>`: the XP loadability check.

Never commit Vista data, secrets or real INI files; `.gitignore` excludes them.

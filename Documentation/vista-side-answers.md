# Vista side: answers to section 8 of vista-erp-import-answers.md

Source: a full copy of Vista's `Tables` folder (Vista 4.31.134, Visual FoxPro), read on 2026-11-24.
Only the Vista company `SO` (SoLasta Corp) is imported. `SM` (Cimtech Corp) and the other Vista companies are out of scope.

| # | Question | Answer |
|---|---|---|
| 1 | Which tenant? | **Solasta Lighting** (`SOLA:3176D:198205F5-C1E1-4D80-8BFB-C7C6688BFDB6`) only. |
| 2 | Units of measure? | Stock unit (`IPART.IPIUM`): `EA` on 39 parts, `FT` on 8. Both map to `each` / `ft`. The purchase unit (`IPPUM`) is `PK` on one part (`MTSO-012`), which is not imported as a unit. No schema change needed. |
| 3 | Price levels, customer prices, breaks now? | No. `SO` has one price list row and one break, both test data (`ACCOUNTINGTEST1`). Load base price (`IPUPRC`) and cost first. |
| 4 | Part numbers unique and short enough? | Yes. 47 live parts, all unique, longest is 19 characters, so they fit both `StockItems.Sku` (30) and `Products.Sku` (20). |
| 5 | Lots, serials, warehouses? | None. The `SO` company has no lot or serial tables, and the location field (`IPLOC`) is blank on every part, so on-hand stock is one number per part. |
| 6 | One-time or ongoing? | Ongoing sync while Vista is still in use: Vista owns SKU, name, category, unit, cost and price; Solasta owns everything else. To be confirmed by Andres. |

## Vista fields for the item import (`IPART`)

| Vista | Meaning | Solasta |
|---|---|---|
| `IPPART` C(20) | Part number | `INE.StockItems.Sku`, plus `ExternalSource = 'VISTA'`, `ExternalId` |
| `IPDESC` C(30) | Description | `Name` |
| `IPCLAS` C(5), described in `ICLAS` | Class: LIGHT, CONTR, TRACK, PACK, TEST | `Category` (free text) |
| `IPIUM` C(2) | Stock unit, EA or FT | `UnitOfMeasure` |
| `IPAMUC` / `IPSMUC` N(12,5) | Average / standard material cost | `StandardCost` (standard when set, otherwise average) |
| `IPUPRC` N(12,5) | Unit selling price | Not mapped yet: needs a decision (`MAC.Products` has no write API and only Rail/Fixed/AddOn categories) |
| `IPQOH` N(12,2) | Quantity on hand | Opening `AdjustIn` movement, later phase. Some parts are negative in Vista (for example `ST-U6-2904-BK` at -527), which Solasta refuses. |
| `IPVEND` C(5) | Default vendor | Blank on every `SO` part |

Suggested to skip: test parts (`ACCOUNTINGTEST1`, `ACCOUNTINGTEST2`, `ACCOUNTINGMATERIAL1`, class `TEST`) and shipping placeholders (`SHIP-001` to `SHIP-008`).

## Connectivity note for the XP machine

Windows XP's own TLS stack (SChannel, used by WinHTTP and WinINet) supports TLS 1.0 at most. `https://api.mysolasta.com` and the identity server expect TLS 1.2.

The design is a **direct call from the XP machine**. The sync service (`vista-sync.exe`, native C++, see `README.md`) runs on the XP machine itself. It carries its own TLS 1.2 stack, mbedTLS 3.6 LTS, compiled in. It does not use SChannel or the XP certificate store; it trusts only the root CAs in its own `ca-bundle.pem`. It never touches the database:

1. it gets an OAuth2 `client_credentials` token from the identity server with the `vista-import` client;
2. it POSTs new and changed items to the new import endpoint of `api.mysolasta.com`, in one batch per run to limit TLS handshakes on the 363 MHz CPU;
3. the API writes to SQL Server on `SOLITMDB001`, with tenant, audit and validation rules applied as for any other caller.

No relay machine, shared folder or direct SQL connection is involved.

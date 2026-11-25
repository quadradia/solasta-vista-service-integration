# Solasta ERP: answers for the Vista ERP (FoxPro) item and pricing import

This document answers the six questions from the import project. The answers come from the code and schema as of 2026-11-24: `quadradia/solasta_database` at `main` 88addf7, and `quadradia/solasta-lighting-est-api` at `main` 498be9d. Section 7 adds the development standards to follow if the import needs database or API changes. It contains no passwords or connection strings.

**Short version:**
- Solasta is SQL Server behind a multi-tenant .NET 10 REST API with OAuth2 bearer tokens.
- Items live in `INE.StockItems`, which is the inventory master. Sellable prices live in `MAC.Products`.
- **There are no price lists, no customer-specific prices, no quantity price breaks and no category or unit-of-measure tables yet.** Units are limited to `ft` and `each`.
- No table has an external or legacy id column for items yet. `Sku` is unique per tenant and is the natural upsert key.
- There's no import endpoint yet, and existing endpoints accept only staff logins. A service needs a small amount of API and database work first (section 1.3).

---

## 1. Engine, hosting, and how an outside service connects

### 1.1 Engine and hosting

| | Dev (local) | Prod |
|---|---|---|
| Engine | Microsoft SQL Server. The schema project `SOL_MAIN/SOL_MAIN/SOL_MAIN.sqlproj` targets `Sql170DatabaseSchemaProvider` (SQL Server 2025 compatibility). Check the exact server build with `SELECT @@VERSION`; the repository doesn't record it. | same |
| Server / database | `DESKTOP-GAHBRKG` / `SOL_MAIN`, Windows auth | `SOLITMDB001` / `SOL_MAIN`, on-premises, SQL login |
| API | `https://localhost:7141`, environment `Development` | `https://api.mysolasta.com`, IIS on `QUADITMAPP001` |
| Token issuer | `https://localhost:7065` | `https://identity.nuvol9.com` (audience `solasta-api`) |

### 1.2 Connection options and the recommendation

**Recommended: the REST API, through import endpoints added for this purpose.** Don't write directly to the database. The reasons come from the code:

- Every API database command is prefixed with `EXEC ACC.spSessionContextCreate @IdentityID, @TenantProxy` (added by `DynamicSqlInjectionInterceptor`). That stores the user and tenant in `SESSION_CONTEXT('UserContext')`.
- The tenant-guard triggers on the INE tables (`tr_*_TenantGuard`) only enforce the tenant when that context exists. A direct connection without it can write rows into any tenant.
- The audit triggers (`trgAutoGen_*_CUD`, which write to `AUD.AuditActions`) and the `CreatedById` and `ModifiedById` columns also take the user from that context. Without it they fall back to the default GUID `E6872B58-52B2-415C-A32B-45805F95A70A`.
- On-hand stock can only change through stored procedures (`INE.spMovementPost` and related). The API already wraps them and turns their `041xx` errors into 400 responses.
- Validation (SKU uniqueness, upper-casing, lengths, references within the tenant) lives in the API facades. See section 4.
- The Prod SQL login is held by one person in a DPAPI-encrypted store, and no service account exists for outside callers.

A **one-time bulk load directly in SQL** is acceptable only as a reviewed script in the database repository. It must:
1. call `EXEC ACC.spSessionContextCreate @IdentityID = <service user GUID>, @TenantProxy = '<tenant proxy>'` on the same connection first;
2. insert master data with plain INSERT or MERGE;
3. post opening stock only through `INE.spMovementPost`, using `AdjustIn` movements.

It's deployed with the same wrapper process as a migration (section 7.3).

### 1.3 How a service authenticates (what exists, and what to add)

- The API accepts JWT bearer tokens from the Identity Server and validates audience `solasta-api`.
- A **client-credentials** token has no `sub` claim. The API maps the token's `client_id` to an `ACC.Users` service user through the setting `ServiceAccounts:<client_id>`; see `Solasta.WebApi.Host/Services/HttpUserContextProvider.cs`.
- The tenant comes from the token's **`tenant_id` claim, which holds the tenant's Proxy string**. For a service client this is a client claim configured in the Identity Server. The website's `marketing-worker` client works this way today.
- The service user must be an **active `ACC.Users` row in that tenant**. Otherwise `ACC.spSessionContextCreate` refuses the request with error 03021 or 03022.
- **The gap:** every inventory controller (`InventorySetupController`, `StockController`, `PurchaseOrdersController`) is `[Authorize(Policy = AuthorizationPolicies.Staff)]`. That policy requires role `staff` and scope `solasta-api`, so a client-credentials token can't call them.

To enable an import service, open one issue and PR per repository:

1. **Database:** a migration seeding a service user with a fixed `UserID` GUID in the target tenant. Copy `Migrations/20261111_1700_SeedACC_MarketingWorkerServiceUser.sql`.
2. **Identity Server** (configured by Andres): a client such as `vista-import`, using client credentials, with scopes `solasta-api` plus a new `inventory.import`, and a client claim `tenant_id` set to the tenant's proxy.
3. **API:**
   - `ServiceAccounts:vista-import` set to the service user GUID in `appsettings.json`;
   - a new policy in `Authorization/AuthorizationPolicies.cs` that requires the new scope;
   - an import controller, for example `Controllers/INE/InventoryImportController.cs`, offering batch upserts that return results per row. These can reuse `InventorySetupFacade` and `StockFacade`.
   
   There is no fallback authorization policy, so a controller without `[Authorize]` is anonymous. Never leave it off.

The existing endpoints the import controller would mirror are all under `api/inventory`:
- vendors: `GET/POST vendors`, `PUT vendors/{id}`
- locations: `GET/POST locations`, `PUT locations/{id}`
- items: `GET/POST items`, `PUT items/{id}`, `GET items/stockable`, and `items/{id}/variants`
- reorder rules: `GET/PUT stock-levels`
- stock: `GET stock`, `POST movements`, `POST movements/{id}/reverse`
- purchase orders: `GET/POST purchase-orders`, `PUT purchase-orders/{id}`, `POST purchase-orders/{id}/status`, `POST purchase-orders/{id}/receipts`
- `GET lookups`

Responses are DSM envelopes. A 400 carries field errors, a 404 means not found, and a 403 means an unknown user or tenant. A server fault comes back as `API-APPVLD-02000` with a 500, so read the API log rather than the label.

---

## 2. Schema for items, pricing, vendors and on-hand stock

Every table has `DealerTenantId int NOT NULL`, a foreign key to `ACC.DealerTenants`, and the **standard audit tail**. In the tables below, `→` marks a foreign key. The audit tail is:

`IsActive bit NOT NULL DF 1, IsDeleted bit NOT NULL DF 0, ModifiedDate datetimeoffset(7) NOT NULL DF now, ModifiedById uniqueidentifier NOT NULL, CreatedDate datetimeoffset(7) NOT NULL DF now, CreatedById uniqueidentifier NOT NULL, DEX_ROW_TS datetimeoffset(7) NOT NULL DF now`

The API treats `IsActive = 0` as deleted.

There are **two item concepts**:
- `INE.StockItems` is what's *stocked* and counted: the inventory master.
- `MAC.Products` is what's *sold*, with a price. Quotes price from it.

A stock item can point at a product through `ProductId`. A Vista part with a sell price most likely becomes one `StockItems` row and, if it is sold on quotes, one `Products` row.

### 2.1 `INE.StockItems`: items (file `Tables/INE/StockItems.sql`)

| Column | Type | Null | Notes |
|---|---|---|---|
| `StockItemID` | int IDENTITY | no | Primary key |
| `DealerTenantId` | int | no | → `ACC.DealerTenants` |
| `ProductId` | int | yes | → `MAC.Products` (the priced product) |
| `ParentStockItemId` | int | yes | → `INE.StockItems`. Set on a **variant**; one level deep only. |
| `VariantName` | nvarchar(50) | yes | Required when a parent is set. Unique per parent. |
| `PriceAdder` | decimal(19,4) | no, default 0 | ≥ 0. Extra per unit added to the product price on quotes. |
| `DefaultVendorId` | int | yes | → `INE.Vendors` |
| `Sku` | nvarchar(30) | **no** | **Unique per tenant** (`UQ_StockItems_DealerTenantId_Sku`). The API upper-cases it. |
| `Name` | nvarchar(150) | **no** | |
| `Description` | nvarchar(1000) | yes | |
| `Category` | nvarchar(50) | yes | **Free text. There is no category table.** |
| `TrackingMode` | varchar(10) | no, default `Quantity` | `Quantity`, `Lot` or `Serial` |
| `UnitOfMeasure` | varchar(4) | no, default `each` | **CHECK: `ft` or `each` only** |
| `VendorSku` | nvarchar(50) | yes | The vendor's part number. It is **not** unique. |
| `StandardCost` | decimal(19,4) | yes | ≥ 0. Last known cost; stock is valued at this. |
| `PodsPerStrand`, `PodSpacingInches` | int, decimal(9,3) | yes | Light-strand items only. Both set or both NULL. |

Other CHECK constraints: a `Serial` item must use `each`, and an item can't be its own parent. Triggers:
- `trgAutoGen_StockItems_CUD` writes the audit log.
- `tr_StockItems_TenantGuard` raises 04180, 04181 or 04182 for tenant violations.
- `tr_StockItems_VariantGuard`: a variant takes its parent's `ProductId`.

Variant rules: a parent never holds stock (04190). An item can get its first variant only while it holds no stock (04192). Variant attributes go in `INE.StockItemVariantAttributes`: `StockItemId`, `AttributeName nvarchar(30)`, `AttributeValue nvarchar(50)`, unique per item and attribute name, and only allowed on variants (04193).

### 2.2 `MAC.Products`: sellable products and the base price (file `Tables/MAC/Products.sql`)

| Column | Type | Null | Notes |
|---|---|---|---|
| `ProductID` | int IDENTITY | no | Primary key |
| `DealerTenantId` | int | no | → `ACC.DealerTenants` |
| `Sku` | **varchar(20)** | no | **Unique per tenant** (`UQ_Products_DealerTenantId_Sku`). Shorter than the stock item SKU, and not Unicode. |
| `Name` | nvarchar(100) | no | |
| `Category` | varchar(10) | no | **CHECK: `Rail`, `Fixed` or `AddOn`.** These are pricing roles in the quote engine, not item classes. |
| `Unit` | varchar(4) | no | **CHECK: `ft` or `each`** |
| `PricePerUnit` | decimal(19,4) | no | ≥ 0. **The one and only sell price.** |
| `InstallCostPerUnit` | decimal(19,4) | yes | ≥ 0. Internal; never shown to customers. |
| `ProductCostPerUnit` | decimal(19,4) | yes | ≥ 0. Internal. |
| `SortOrder` | int | no, default 0 | |
| `IsPublic` | bit | no, default 1 | Shown to the public AI quoting tool |

`MAC.Products` has **no create or update API**. It's read through `GET api/quote-catalog`, and its rows were seeded by migrations: `20261117_0330_CreateMAC_PricingCatalog` and `20261117_2230_AddMAC_QuoteBuilder`. Quotes copy prices when they are created, so changing a price never changes an existing quote. The API's hand-written partial is `Solasta.Data/Models/MAC/PricingCatalog.cs`, which defines the allowed values.

### 2.3 Pricing structures that exist, and what's missing

| Concept | Status |
|---|---|
| Base sell price | `MAC.Products.PricePerUnit`, plus the `INE.StockItems.PriceAdder` of a variant |
| Promotions | `MAC.Promotions`: `Code varchar(20)` unique per tenant, `Name nvarchar(100)`, `AmountPerUnit decimal(19,4)` (≥ 0, taken off **each foot of rail**), `StartDate`/`EndDate date`, `MaxValidityHours int`, `IsPublic`. This is specific to rail; it is not a general discount. |
| Tax | `MAC.TaxRates`: `City`, `County`, `StateCode char(2)`, `Rate decimal(9,6)` (0 to 1), `EffectiveDate`, `IsDefault`. Unique per (tenant, state, city). |
| Financing | `MAC.FinancingPlans` (APR and terms for quotes) |
| **Price lists or price levels** | **None** |
| **Customer-specific prices** | **None.** Customers are `ACE.Customers` (`CustomerID bigint`), and no price table references them. |
| **Quantity price breaks** | **None** |
| **Item categories or classes table** | **None.** `StockItems.Category` is free text up to 50 characters. |
| **Units of measure table or conversions** | **None.** Only `ft` and `each`, enforced by CHECK constraints on `StockItems`, `Products` and the quote engine. |

If Vista's price lists, customer prices, breaks, classes or units must come across, they need **new tables** (a migration, then an AutoGenerator run, then API code; section 7). Widening the unit-of-measure CHECK constraints also affects the quote pricing engine (`Solasta.Business/Services/MAC/QuotePricingCalculator.cs`), which assumes `ft` and `each`.

### 2.4 `INE.Vendors` (file `Tables/INE/Vendors.sql`)

| Column | Type | Null | Notes |
|---|---|---|---|
| `VendorID` | int IDENTITY | no | Primary key |
| `DealerTenantId` | int | no | → `ACC.DealerTenants` |
| `PoliticalStateId` | varchar(4) | yes | → `MAC.PoliticalStates` (the API takes a state code) |
| `Code` | nvarchar(20) | **no** | **Unique per tenant** (`UQ_Vendors_DealerTenantId_Code`) |
| `Name` | nvarchar(150) | **no** | |
| `ContactName` nvarchar(100), `Email` nvarchar(256), `Phone` nvarchar(30), `Website` nvarchar(256) | | yes | The API checks that the email is valid |
| `StreetAddress`, `StreetAddress2` nvarchar(100), `City` nvarchar(100), `PostalCode` nvarchar(15) | | yes | |
| `LeadTimeDays` | int | yes | ≥ 0 |
| `PaymentTerms` | nvarchar(50) | yes | Free text |
| `Notes` | nvarchar(2000) | yes | |

### 2.5 Locations and on-hand stock

**`INE.Locations`** (file `Tables/INE/Locations.sql`):

| Column | Type | Notes |
|---|---|---|
| `LocationID` | int IDENTITY | Primary key |
| `LocationTypeId` | int | → `INE.LocationTypes`, seeded with fixed ids |
| `UserId`, `LeadId`, `VendorId` | | Optional links |
| `Code` | nvarchar(30) | Unique per tenant. `JOB-` and `VND-` prefixes are reserved. |
| `Name` | nvarchar(150) | |
| Address fields | | |

Location types: `Warehouse`, `Vehicle` and `Staging` hold stock; `Job` and `Vendor` don't.

**`INE.StockBalances`** holds on-hand stock. **It's read-only to everyone except the INE procedures.**

| Column | Type | Notes |
|---|---|---|
| `StockBalanceID` | bigint | Primary key |
| `LocationId`, `StockItemId`, `LotId` (nullable) | | Unique together |
| `QuantityOnHand` | decimal(19,4) | ≥ 0 |
| `QuantityAllocated` | decimal(19,4) | 0 ≤ allocated ≤ on hand |
| `LastMovementId` | bigint | |

It has no `IsActive`/`CreatedById` audit columns.

**`INE.Lots`**: `LotNumber nvarchar(50)`, unique per item; `ManufacturedDate`, `ExpirationDate`.

**`INE.SerialUnits`**: `SerialNumber nvarchar(60)`, unique per item. `Status` is one of `InStock`, `Installed`, `Returned`, `Scrapped`, `Missing`, `Voided`. Also `CurrentLocationId`, `UnitCost`.

**On-hand stock is a ledger projection.** It's changed only by posting movements to `INE.Movements` and `INE.MovementLines`, which are append-only, through `INE.spMovementPost @MovementJson`. **Load Vista quantities as `AdjustIn` movements** into a location that holds stock:

```json
{ "movementType": "AdjustIn", "toLocationId": 1, "reference": "VISTA-OPENING-2026-12-01",
  "lines": [ { "stockItemId": 3, "quantity": 164.5 },
             { "stockItemId": 5, "quantity": 40, "lotNumber": "R-2611-07" },
             { "stockItemId": 9, "serialNumbers": ["C8-0001", "C8-0002"] } ] }
```

Through the API, this is `POST api/inventory/movements`. Corrections are a `Reversal` (`POST movements/{id}/reverse`), never an edit. Read stock from the views `INE.vwStockOnHand` (on hand, allocated and available per location, item and lot), `vwMovementHistory`, `vwReorderSuggestions` and `vwStockBalanceDrift` (which must be empty). The views are **not** filtered by tenant, so always filter on `DealerTenantId`.

**Reorder rules** are in `INE.StockLevels`: `StockItemId`, `LocationId` (unique together), `ReorderPoint decimal(19,4)` ≥ 0, `MaximumQuantity` ≥ reorder point.

**Purchase orders** are in `INE.PurchaseOrders`:
- `VendorId`, `ShipToLocationId`;
- `PONumber nvarchar(30)`, assigned by the API as `PO-{n}`;
- `Status`, starting at `Draft`, which only `spPurchaseOrderSetStatus` and receipts change;
- `OrderDate`, `ExpectedDate`, `VendorReference`, `ShippingAmount`, `TaxAmount`.

Its lines are in `INE.PurchaseOrderLines`: `LineNumber` (unique per PO), `QuantityOrdered` > 0, `QuantityReceived` ≤ ordered, `UnitCost` ≥ 0. Lines are locked after Draft (04171).

### 2.6 Relationships

```
ACC.DealerTenants 1─* everything below (DealerTenantId)
MAC.Products      1─* INE.StockItems (ProductId)
INE.Vendors       1─* INE.StockItems (DefaultVendorId), 1─* INE.PurchaseOrders
INE.StockItems    1─* INE.StockItems (variants), 1─* StockItemVariantAttributes, Lots, SerialUnits, StockLevels, StockBalances, MovementLines, PurchaseOrderLines
INE.Locations     1─* StockBalances, StockLevels, Movements (From/To), PurchaseOrders (ShipTo)
INE.Lots          1─* StockBalances, SerialUnits, MovementLines
```

**Legacy tables to ignore:** `MAC.PurchaseOrders`, `MAC.PurchaseOrderTypes` and `MAC.Customers` predate INE and ACE. The generated `PurchaseOrderLine` navigation in the API points at the wrong class (`MAC.MACPurchaseOrder`), so query lines by `PurchaseOrderId` instead.

---

## 3. Identifiers, external ids and re-runnable upserts

- Primary keys are `IDENTITY` integers: `int` for master data, `bigint` for ledger and PO rows. **They differ between Dev and Prod.** Never carry them between environments; resolve them by natural key at run time.
- Public-facing rows (tenants, leads, quotes, invoices) also have a **Proxy** string `Prefix:PartKey:GUID`. INE and `MAC.Products` rows have no proxy.
- **Natural keys, unique per tenant, that you can upsert on today:**

  | Table | Natural key |
  |---|---|
  | `INE.StockItems` | `Sku` (nvarchar 30, stored upper-case by the API) |
  | `MAC.Products` | `Sku` (varchar 20) |
  | `INE.Vendors` | `Code` (nvarchar 20) |
  | `INE.Locations` | `Code` (nvarchar 30) |
  | `MAC.Promotions` | `Code` (varchar 20) |
  | `INE.Lots` | `LotNumber`, per item |
  | `INE.SerialUnits` | `SerialNumber`, per item |
- **There is no external or legacy id column on any item, product or vendor table.** `StockItems.VendorSku` is the *supplier's* part number. It isn't unique and shouldn't hold the Vista number.
- **The established pattern for external ids** is on `QAL.Leads`:
  - columns `ExternalSource varchar(20) NULL` and `ExternalId nvarchar(100) NULL`;
  - check constraint `CK_Leads_ExternalSource CHECK (ExternalId IS NULL OR ExternalSource IS NOT NULL)`;
  - filtered unique index `IX_Leads_DealerTenantId_ExternalSource_ExternalId ON (DealerTenantId, ExternalSource, ExternalId) WHERE ExternalId IS NOT NULL`.
  
  Upserts match on the external id first, then on a natural key (email), and only fill fields that are missing (`LeadFacade`).
- **Recommendation for Vista:**
  - If the Vista part number fits in 30 characters and is unique, use it as `Sku`.
  - Either way, add `ExternalSource`/`ExternalId` to `INE.StockItems`, `MAC.Products` and `INE.Vendors` with a migration that follows the Leads pattern, and use `ExternalSource = 'VISTA'`.
  - Match on (tenant, `VISTA`, part number), then on `Sku`. Insert when nothing matches; otherwise update only the fields Vista owns.
- **Stock quantities are not upserted.** To make opening balances safe to re-run, give each movement a deterministic `reference` (for example `VISTA:<batch>:<location>`) and check `INE.vwMovementHistory` for that reference before posting. To change a posted balance, post a Reversal, or an adjustment for the difference.
- **Load order** follows the foreign keys:
  1. vendors
  2. locations
  3. products (optional)
  4. stock items (parents, then variants and their attributes)
  5. reorder rules
  6. opening stock (`AdjustIn`), or POs and receipts

---

## 4. Existing import, sync and seed code, and validation rules

### 4.1 Import, sync and seed code that exists

- **No inventory or product import or sync exists.** No code reads FoxPro or Vista.
- Seed data is in migrations and `Data/<SCHEMA>/*_data.sql`, using `MERGE` or a conditional `INSERT` so it's safe to re-run:
  - `Data/INE/LocationTypes_data.sql`, `Data/INE/MovementTypes_data.sql` (fixed ids);
  - the products and promotions inserted by `Migrations/20261117_0330_CreateMAC_PricingCatalog.sql` and `20261117_2230_AddMAC_QuoteBuilder.sql`.
- `INE.spStockItemConvertToVariants` (migration `20261120_0300`) turns an item that holds stock into a parent plus its first variant, moving the stock.
- Patterns for other syncs, useful as models:
  - `MetaLeadSyncWorker` and `MetaLeadFacade`: a scheduled re-read plus upsert by external id;
  - `SocialPostSyncWorker`.
  
  Both run as a service user through `BackgroundIdentity.Use(userId, tenantProxy)`.
- `Scripts/Dev/INE_ResetLocal.sql` and `Scripts/Tests/INE/INE_EndToEnd_Tests.sql` (47 checks) are for Dev and throwaway copies only.

### 4.2 Validation when items are created or changed (API: `Solasta.Business/Facades/INE/InventorySetupFacade.cs`)

**Stock items:**
- `Sku` is required, trimmed, at most 30 characters, **upper-cased**, and unique per tenant ("Another stock item already uses the SKU …").
- `Name` is required, at most 150 characters.
- `Description` is at most 1000 characters, `Category` at most 50 and `VendorSku` at most 50.
- `TrackingMode` must be Quantity, Lot or Serial; it defaults to Quantity and is case-insensitive.
- `UnitOfMeasure` must be `ft` or `each`; it defaults to `each`. A Serial item must use `each`.
- **Tracking mode and unit can't change once the item has stock movements.**
- `StandardCost` must be ≥ 0 and is rounded to 4 decimals, half away from zero.
- `ProductId` and `DefaultVendorId` must exist in the same tenant; 0 clears them.
- A variant's product follows its parent.
- An item can't be deactivated while it has stock on hand at a location that holds stock.
- Light-strand fields are both set or both cleared: 1 to 10000 pods, more than 0 and at most 1000 inches, `each` only.

**Vendors:**
- `Code` is required, at most 20 characters, unique per tenant. `Name` is required, at most 150.
- `Email` must be a valid address. The state must be a known state code. `LeadTimeDays` must be ≥ 0.
- Maximum lengths: contact 100, phone 30, website 256, street 100, city 100, postal code 15, terms 50, notes 2000.

**Locations:** `Code` is required, at most 30 characters, unique per tenant, and can't use the `JOB-` or `VND-` prefixes.

**Reorder rules:** the maximum must be at least the reorder point, and parents with variants are refused.

**Database-level rules** apply on every write path:
- the CHECK constraints listed in section 2;
- the tenant guards (04180 to 04182);
- the variant guards (04190 to 04196);
- stock can't go negative (`CK_StockBalances_*`, error 04122, "Not enough available stock");
- the ledger is append-only (04170).

Procedure errors use the format `NNNNN|:|message`, wrapped by `GEN.spExceptionsThrown`. The full code list is in `docs/INE-Inventory-Engine.md`, section 7.

**Pricing:** prices are copied onto quotes and never repriced. All rounding is half away from zero (Excel style), never banker's rounding.

---

## 5. Multiple companies and tenants

Yes. Solasta is multi-tenant in a single database.

- Tenants are rows in `ACC.DealerTenants`. Each has a `DealerTenantID int` primary key, a `Proxy` (for example Solasta Lighting is `SOLA:3176D:198205F5-C1E1-4D80-8BFB-C7C6688BFDB6`, and Cimtech Manufacturing is `CIMT:31772:E1518BD7-D0B7-4472-85C2-AAC7A8E42EBB`), a `DealerPrefix` and a type in `ACC.DealerTenantTypes`. Proxies are the same in every environment and are not secret.
- Every business row carries `DealerTenantId`. Uniqueness, such as SKU or vendor code, is **per tenant**, so two tenants can use the same SKU.
- The tenant comes from the token (`tenant_id` claim, a proxy), **never from the request body**. Users belong to a tenant (`ACC.Users.DealerTenantId`) or are granted others through `ACC.UserTenantMemberships`.
- The database enforces isolation through the session context (`ACC.spSessionContextCreate`) and the INE tenant-guard triggers. The API also filters every query by the caller's `DealerTenantId`.
- **An import runs for one tenant per token or session.** To load the same Vista data into two tenants, run it once per tenant, with a service user in each.

---

## 6. Repositories and paths

| What | Repository and path |
|---|---|
| Database (source of truth) | `quadradia/solasta_database` (branch `main`) |
| Migrations | `Migrations/YYYYMMDD_HHMM_*.sql`, especially `20261117_1800_CreateINE_InventoryEngineTables.sql`, `20261117_1810_CreateINE_InventoryEngineProcedures.sql`, `20261117_1900_AddINE_AuditAndTenantGuards.sql`, `20261120_0100_AddINE_PartVariants.sql`, `20261120_0300_AddINE_ConvertToVariantsAndQuoteColorChoice.sql`, `20261117_0330_CreateMAC_PricingCatalog.sql`, `20261117_2230_AddMAC_QuoteBuilder.sql`, `20261026_2352_SeedACC_CimtechManufacturingTenant.sql`, `20261111_1700_SeedACC_MarketingWorkerServiceUser.sql` |
| Current object definitions | `Tables/INE/*.sql`, `Tables/MAC/Products.sql`, `Promotions.sql`, `TaxRates.sql`, `FinancingPlans.sql`, `Tables/ACC/DealerTenants.sql`, `Users.sql`, `Tables/QAL/Leads.sql` (external id pattern), `Procedures/INE/*.sql`, `Triggers/INE/*.sql`, `Views/INE/*.sql`, `Data/INE/*.sql` |
| Inventory design document | `docs/INE-Inventory-Engine.md` |
| Rules and deploy guides | `DATABASE_CONSTITUTION.md`, `docs/Deploying-Migrations.md`, `Migrations/_TEMPLATE_Migration.sql`, `Scripts/deploy/Deploy_*.ps1` |
| API | `quadradia/solasta-lighting-est-api` (branch `main`), solution `Source/Solasta/Solasta.slnx`, rules in `CLAUDE.md` |
| Inventory API | `Solasta.WebApi.Host/Controllers/INE/*.cs`, `Solasta.Business/Facades/INE/InventorySetupFacade*.cs`, `StockFacade.cs`, `PurchaseOrderFacade.cs`, `Solasta.Business/Models/INE/InventoryDtos.cs`, `Solasta.DataAccess/UnitOfWork.INE.cs` |
| Generated models | `Solasta.Data/Models/INE/*.generated.cs`, `Solasta.Data/Models/MAC/Product.generated.cs` (+ `PricingCatalog.cs`) |
| Auth | `Solasta.WebApi.Host/Authorization/AuthorizationPolicies.cs`, `Services/HttpUserContextProvider.cs`, `appsettings.json` (`ServiceAccounts`) |
| AutoGenerator | `SosaNuvol/nuvol9-agc-dotnet`, profile `profiles/solasta/agc.profile.json`, rules in `CLAUDE.md` |

---

## 7. Development standards for database and API changes

### 7.1 Work tracking

- Every change starts with a **GitHub issue** in the repository being changed. The **PR** body says `Closes #<issue>`. The default branch is `main`.
- One PR per repository. A typical feature touches the database repository (migration), the API (generated code plus hand-written code) and sometimes the admin app.
- **No secrets in git, chat or command lines.**
  - API: `dotnet user-secrets` locally; IIS app pool environment variables in Prod, set with `deploy/Set-SolastaApiProdSecret.ps1`.
  - AutoGenerator: `%APPDATA%\Nuvol9\AGC\connections.json`.
  - Database deploys: `Save-DeployCredential.ps1`, which uses DPAPI.

### 7.2 Database changes (`solasta_database`)

**Migration files.** Each change is a migration `Migrations/YYYYMMDD_HHMM_Description.sql`, with a UTC timestamp. A migration deployed beyond Dev is never edited. Every migration:
1. starts with a header (name, author, date, description, dependencies, expected impact);
2. skips itself if `dbo.SchemaVersion` already has its name with `Success = 1`;
3. wraps everything in `SET XACT_ABORT ON; BEGIN TRANSACTION; BEGIN TRY … COMMIT … END TRY BEGIN CATCH ROLLBACK; THROW; END CATCH`;
4. checks existence before every DDL statement (`IF OBJECT_ID … IS NULL`) and uses `MERGE` or a conditional `INSERT` for data, so it's safe to run twice;
5. inserts its row into `dbo.SchemaVersion`;
6. ends with a commented-out rollback script.

**Object files.** Update the object's file under `Tables/<SCHEMA>/`, `Procedures/`, `Triggers/`, `Views/` or `Data/` (one object per file). Its header lists the migrations that touched it.

**Naming.**
- Tables: PascalCase, plural, `[schema].[Table]`.
- Primary key column `<Table>ID`; foreign key columns `<Ref>Id`.
- Constraints and indexes: `PK_`, `FK_<Table>_<Ref>`, `UQ_`, `CK_`, `DF_`, `IX_`.
- Bracket every identifier and qualify every object with its schema.

**Table shape.**
- Column order: primary key, then foreign keys, then business columns, then the audit tail. The tail is `IsActive, IsDeleted, ModifiedDate, ModifiedById, CreatedDate, CreatedById, DEX_ROW_TS`, with `uniqueidentifier` user ids, because the API's `HistoricalEntity` maps them as Guid. An `int` there builds but throws `InvalidCastException`.
- A table with no identity column gets `DEX_ROW bigint IDENTITY` as its last column.
- New tenant tables need `DealerTenantId` with a foreign key to `ACC.DealerTenants`.
- Types: `NVARCHAR` for text, `DECIMAL(19,4)` for money and quantities, `DATETIMEOFFSET(7)` for timestamps.

**Triggers and errors.** Tenant guards and `trgAutoGen_<Table>_CUD` audit triggers follow the INE examples. Procedure errors use `NNNNN|:|message` through `GEN.spExceptionsThrown`.

**Deploy wrapper.** Each change gets `Scripts/deploy/Deploy_<YYYYMMDD_HHMM>_<SCHEMA>_<Change>.ps1`, copied from the newest wrapper (`Deploy_20261123_1530_ACE_Appointments.ps1`). It:
- runs `Deploy_Migrations.ps1` with `-MigrationFilter` set to the exact file and `-StopOnError`;
- checks its prerequisites are in `SchemaVersion`;
- supports `-WhatIf`;
- for Prod, makes you type `SOLITMDB001/SOL_MAIN` (or pass `-ConfirmTarget`);
- runs post-checks that the migration is recorded and its objects exist.

**Never run `Deploy_Migrations.ps1` on its own against Prod.**

**Testing.** Run each migration twice on Dev (or on a restored copy, such as `SOL_TEST`) to prove it's idempotent. INE changes also run `Scripts/Tests/INE/INE_EndToEnd_Tests.sql` in a throwaway copy.

### 7.3 AutoGenerator (AGC), required after every database change

Project rule: run AGC after any database change and commit its output. **Never hand-write mappings for generated tables, and never edit `*.generated.cs` files.**

1. Deploy the migration to Dev.
2. Run `RQI.POC.AutoGenConsole.exe --profile solasta --dry-run`, then run it without `--dry-run`. A new schema must first be added to `schemas.tables` in `profiles/solasta/agc.profile.json`; today the list is ACC, AFL, ACE, INE, MAC, MCH, MSG, QAL, RSU.
3. The output goes to the API:
   - models: `Solasta.Data/Models/<SCHEMA>/*.generated.cs`
   - user-defined table types: `Solasta.Data/UDDTs/<SCHEMA>/`
   - SPROC wrappers: `Solasta.DataAccess/SPROCS/<SCHEMA>/SP*.generated.cs`
   - DbContext and repositories: `Solasta.Data.Repository`
   - `UnitOfWork.generated.cs` and `IUnitOfWork.generated.cs`
   
   Only files that changed are rewritten.
4. Custom code goes in partial classes or separate files, such as `PricingCatalog.cs` or `Models/DOC/Document.cs`.
5. Tables with triggers must be listed in `SolastaDbContext.TableTriggers`, or EF inserts fail with SQL error 334.

### 7.4 API changes (`solasta-lighting-est-api`)

- .NET 10, database-first, with no EF migrations, and never SQL scripts in this repository.
- Layers:
  - controllers (`Controllers/<SCHEMA>`) stay thin;
  - facades (`Solasta.Business/Facades/<SCHEMA>`) hold the rules and return `DSMEnvelope<T>`;
  - data access goes through `IUnitOfWork`;
  - stored procedures are called through the generated wrappers in `UnitOfWork.<Area>.cs`.
- **Auth:** every controller gets `[Authorize(Policy = …)]`. Take the tenant and user from the token, never from the request.
- **DSM logging rules:**
  - Every method that owns an envelope starts it with `DsmFlow.Begin<T>(nameof(Class))` (in controllers, `HttpContext.BeginDsm<T>(…)`).
  - On every exit path it ends the envelope exactly once with `Succeed`, `Fail`, `Invalid`, `Error` or `FailWith(child)`.
  - A caller checks its callee's envelope.
  - `DsmRulesSourceTests` rejects raw DSM calls.
- **NLog files:**
  - `logs/solasta-api-<date>.log` is the general log. `logs/dsm/dsm-<date>.log` and `.jsonl` hold DSM output only.
  - Locally they are under `bin/Debug/net10.0/logs`. In Prod they are in `D:\Inetpub\api.mysolasta.com\logs`, readable from Andres's PC at `\\Quaditmapp001\d$\Inetpub\api.mysolasta.com\logs`.
  - `SOLASTA_LOG_DIR` overrides the folder.
- **Build and test:**
  - `dotnet build Source/Solasta/Solasta.WebApi.Host/Solasta.WebApi.Host.csproj`
  - `dotnet test Source/Solasta/Solasta.Tests/Solasta.Tests.csproj` (xUnit and Moq, with folders per schema, for example `Solasta.Tests/INE`)
  - Add tests for new facade rules. The `NU1902` and `NU1903` warnings are known.
- **Rounding:** money and quantities round half away from zero, never C#'s default banker's rounding.

### 7.5 Deploy order

Each step goes to Dev first:

1. Database migration on Dev.
2. AGC regeneration, then the API build and tests.
3. PRs merged.
4. Prod database: back it up, run the wrapper with `-WhatIf`, then run it. This needs Andres's explicit go.
5. Prod API: `deploy/Publish-SolastaApiProd.ps1`.
6. Apps (admin), then the import service.

---

## 8. Open questions for the Vista side

1. Which tenant does this data belong to: Cimtech Manufacturing, Solasta Lighting, or both?
2. Which Vista units of measure appear? Anything other than feet or each needs a schema change.
3. Must Vista price levels, customer prices and quantity breaks come across now, or can the import load the base price and cost first and add those later as new tables?
4. Are Vista part numbers unique and at most 30 characters (20 if they become `MAC.Products` SKUs)?
5. Does Vista use lot or serial tracking, and is on-hand stock per warehouse?
6. Is this a one-time cutover or an ongoing sync? Which system owns each field afterwards?

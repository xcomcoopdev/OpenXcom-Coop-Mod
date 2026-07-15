# PR comment: how the two branches were combined

(Paste-ready comment for the `host-authoritative-save-fixes` PR.)

---

Combined this branch with the work from `feat/host-authoritative-save` as requested. To keep the history honest about where the ideas came from, the combined result is built **on top of this branch** — your commit stays the base, with the feature-branch work and a new migration commit layered above it.

We ended up solving several of the same problems independently, which was genuinely useful: where both branches had a fix for the same bug, it validated the diagnosis, and the combined version keeps whichever variant covered the most cases (in two places that's a merge of both). Full mapping below so nothing silently disappears.

| Fix in this branch | Where it lives in the combined branch |
|---|---|
| Remove `Options::HostSaveProgress` / `_host_save_progress` | Kept — both branches removed it. The TestServer `set_option` shim still accepts the name (and ignores it) so older test scripts keep running. |
| Defer the host save until the client blob arrives (`temp_filename`) | Kept, via the same mechanism under a different name (`pendingHostSaveName` + `armDeferredSave`/`clearDeferredSave`). That variant also clears the pending name on cancel and on any world load — we found in testing that a quickload during the save round-trip could otherwise write the wrong world over the named save. Your placement of the actual write (in `writeHostMapSaveProgressFile`, after the blob lands, both games paused) is exactly where the combined write happens — your comment describing that invariant is kept on the function. |
| Embed the client blob in the host `.sav` (`setCoopClientSaveBlob`) | Kept as the keyed multi-entry form (`coopClientSaves: [{key, blob}]`): same single-authority idea, but it preserves *which* client and *which* saveID each blob belongs to, and leaves room for more than one client later. To your comment asking what `coopClientSaveKey` was for — exactly that: it carries the client name + saveID so a blob can never be served to the wrong player or campaign. |
| **v1.8.4 fallback: serve the client world from the on-disk `host_<id>_<name>.data` when the embed is empty** | **Adopted and extended** — this was the one gap the feature branch had (it refused old saves outright). The combined branch turns your fallback into a one-time load-time migration: a legacy save loads, any v1 embed and disk sidecars are imported into the served store, the roster is synthesized from the imported names, and the next save writes the modern embed so the sidecars are never needed again (files are left on disk untouched). Covered by a new harness test (`test_legacy_migration.py`). If you have a real v1.8.4 campaign save around, a smoke test with it would be a great extra check — the harness fixture is a reconstruction. |
| Drop the old validation flow in `writeHostMapSaveProgressFile` ("old spaghetti") | The confusing flow you flagged is gone, but a validation step stays, reordered to validate **before** installing: review testing showed a corrupt client upload could otherwise replace the last good blob and end up embedded in the save. Failure still shows the existing error popup and now leaves the previous good blob served. |
| Stream the client world from memory (`memoryStream`) | Kept — both branches moved to RAM streaming; the combined version streams from the keyed store so the same path serves any client. |
| Store `battleclient` in `coopFilesHost` (was `coopFilesClient`) | Same outcome — `writeHostMapFile` was restructured so the host side always stores into `coopFilesHost`. |
| Comment out the PvP `no_bases` path pending a real bases option | `no_bases` is kept functional in the combined branch so New Battle/PvP behaves as before. Fully agree with your note that a host-menu option (separate vs shared bases) is the right long-term shape — proposing that as a follow-up issue rather than losing the thought in a merge. |
| PauseState Save-button visibility cleanup | Same goal, one step further: Load/Save visibility (and the quickload/quickload gates) all derive from a single predicate now, so the rules can't drift between menus. |
| MonthlyReport autosave gate simplification | Equivalent in both branches (host-only autosave in co-op); kept. |
| Profile OK gate simplification (`saveID != 0`) | Same gate, plus the session teardown now resets `saveID` and the blob store — without that, a leftover ID from an earlier campaign session could trigger the campaign fetch when joining a plain New Battle host. |
| Your design notes ("coopFiles maps = temp transport only, no permanent saves") | Kept as the documentation on the maps in `connectionTCP.h` — the combined architecture follows exactly that rule (embed in the `.sav` is the durable copy; the maps are session scratch). |

Everything else from `feat/host-authoritative-save` (campaign lobby flow, resume/rejoin, client-zero-disk, the harness suite) is unchanged on top.

Test status: full `tools/coop_test` suite + the new `test_legacy_migration.py` green on the combined branch.

---

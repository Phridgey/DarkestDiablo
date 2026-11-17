Each file here defines `patch(t)` and changes vanilla tables through `t.table("<name>")`.
Files run in name order, so start their names with numbers: `010_den_of_evil.py`, `020_...`.
Only the tables a patch touches end up in the build.

Example, showing the API only:

    def patch(t):
        levels = t.table("levels")
        den = levels.first("Name", "Den of Evil")
        den["MonDen"] = "100"   # change one cell

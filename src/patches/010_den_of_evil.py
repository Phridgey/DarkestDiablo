"""Den of Evil: a short forced tunnel that ends in Corpsefire's chamber.

Step 1 (this patch): make the generated cave smaller. The Den's maze settings live in
lvlmaze.txt (row "Act 1 - Cave 1", level 8). Corpsefire and his 5-zombie pack come from
superuniques.txt unchanged. The large end chamber will come later as a hand-made map piece.
"""

DEN_LEVEL_ID = "8"
MAZE_CELL_SIZE = "12"  # vanilla 24


def patch(t):
    maze = t.table("lvlmaze")
    den = maze.first("Level", DEN_LEVEL_ID)
    den["SizeX"] = MAZE_CELL_SIZE
    den["SizeY"] = MAZE_CELL_SIZE

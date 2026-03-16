SIZE = 15  # Board size (15x15)


def create_board():
    """Create an empty SIZE x SIZE board."""
    return [["." for _ in range(SIZE)] for _ in range(SIZE)]


def print_board(board):
    """Print the board with row and column indexes."""
    # Column header
    header = "   " + " ".join(f"{c:2d}" for c in range(1, SIZE + 1))
    print(header)
    for i, row in enumerate(board, start=1):
        # Row number on the left
        print(f"{i:2d} " + " ".join(row))


def in_bounds(r, c):
    return 0 <= r < SIZE and 0 <= c < SIZE


def has_five_in_a_row(board, r, c, symbol):
    """Check whether placing symbol at (r, c) creates five in a row."""
    directions = [
        (1, 0),   # vertical
        (0, 1),   # horizontal
        (1, 1),   # diagonal \
        (1, -1),  # diagonal /
    ]

    for dr, dc in directions:
        count = 1  # the stone just placed

        # count in positive direction
        nr, nc = r + dr, c + dc
        while in_bounds(nr, nc) and board[nr][nc] == symbol:
            count += 1
            nr += dr
            nc += dc

        # count in negative direction
        nr, nc = r - dr, c - dc
        while in_bounds(nr, nc) and board[nr][nc] == symbol:
            count += 1
            nr -= dr
            nc -= dc

        if count >= 5:
            return True

    return False


def board_full(board):
    """Return True if no empty cells remain."""
    for row in board:
        if "." in row:
            return False
    return True


def ask_move(player_symbol):
    """Ask the current player for a move, return as (row, col) 0-based."""
    while True:
        raw = input(
            f"Player {player_symbol}, enter your move as 'row col' (1-{SIZE}), "
            "or 'q' to quit: "
        ).strip()

        if raw.lower() in {"q", "quit", "exit"}:
            return None

        parts = raw.split()
        if len(parts) != 2:
            print("Invalid format. Please enter two numbers, e.g. '8 8'.")
            continue

        try:
            r = int(parts[0])
            c = int(parts[1])
        except ValueError:
            print("Row and column must be integers.")
            continue

        if not (1 <= r <= SIZE and 1 <= c <= SIZE):
            print(f"Row and column must be between 1 and {SIZE}.")
            continue

        return r - 1, c - 1


def play_game():
    board = create_board()
    current_symbol = "X"  # Player 1 starts

    print("Welcome to Gomoku (Five in a Row)!")
    print(f"Board size: {SIZE} x {SIZE}")
    print("Player X goes first, Player O goes second.")
    print("Empty cells are shown as '.'.")
    print_board(board)

    while True:
        move = ask_move(current_symbol)
        if move is None:
            print("Game aborted by user.")
            return

        r, c = move
        if board[r][c] != ".":
            print("That cell is already occupied. Please choose another one.")
            continue

        board[r][c] = current_symbol
        print_board(board)

        if has_five_in_a_row(board, r, c, current_symbol):
            print(f"Player {current_symbol} wins!")
            break

        if board_full(board):
            print("The board is full. It is a draw.")
            break

        # Switch player
        current_symbol = "O" if current_symbol == "X" else "X"


if __name__ == "__main__":
    while True:
        play_game()
        again = input("Play again? (y/n): ").strip().lower()
        if again not in {"y", "yes"}:
            print("Goodbye.")
            break


import csv


def load(path):
    """Reads the rows of a CSV file."""
    with open(path, newline="") as source:
        return list(csv.DictReader(source))


def total(rows):
    # TODO: skip rows without an amount
    return sum(float(row["amount"]) for row in rows)


class Report:
    """A report on the rows of one file."""

    def __init__(self, path):
        self.rows = load(path)

    def render(self):
        return f"{len(self.rows)} rows, {total(self.rows):.2f} in all"

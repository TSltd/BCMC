from collections import Counter


def column_loads(weights, N):
    """
    Compute BCMC column occupancies directly.
    """

    loads = [0] * N

    offset = 0

    for w in weights:

        for j in range(w):
            loads[(offset + j) % N] += 1

        offset = (offset + w) % N

    return loads


def prefix_walk(weights, N):
    """
    Compute the cumulative prefix-sum walk.
    """

    walk = [0]

    s = 0

    for w in weights:
        s = (s + w) % N
        walk.append(s)

    return walk


def analyse(weights, N):

    print("=" * 70)

    print(f"N = {N}")
    print(f"Weights = {weights}")

    print()

    walk = prefix_walk(weights, N)

    print("Prefix walk")
    print("-----------")

    for i in range(len(weights)):
        print(
            f"{i:2d}: "
            f"{walk[i]:2d} -> {walk[i+1]:2d}"
            f"   len={weights[i]}"
        )

    print()

    loads = column_loads(weights, N)

    print("Column occupancies")
    print("------------------")

    for i, L in enumerate(loads):
        print(f"{i:2d}: {L}")

    print()

    W = sum(weights)

    q, r = divmod(W, N)

    print(f"Total weight W = {W}")
    print(f"W = {q}×{N} + {r}")
    print()

    print(f"Expected occupancies: {q} or {q+1}")
    print(f"Observed occupancies: {sorted(set(loads))}")
    print()

    hist = Counter(loads)

    print("Histogram")
    print("---------")
    print(hist)

    print()

    higher = hist[q + 1]
    lower = hist[q]

    print(f"Columns with {q}   : {lower}")
    print(f"Columns with {q+1} : {higher}")

    print()

    if higher == r and lower == N - r:
        print("✓ Histogram matches quotient/remainder exactly.")
    else:
        print("✗ Histogram does NOT match quotient/remainder.")


if __name__ == "__main__":

    analyse([6, 3, 5], 8)

    print()

    analyse(
        [4, 13, 13, 11, 12, 5, 10, 2, 0, 12],
        15
    )

    print()

    analyse(
        [24, 26, 28, 11, 8, 20, 19, 26, 2, 30],
        31
    )
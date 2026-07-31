from collections import Counter


def column_loads(weights, N):

    loads = [0] * N

    s = 0

    for w in weights:

        for j in range(w):
            loads[(s + j) % N] += 1

        s = (s + w) % N

    return loads


def prefix_walk(weights, N):

    walk = [0]

    s = 0

    for w in weights:
        s = (s + w) % N
        walk.append(s)

    return walk


def analyse(weights, N):

    print("=" * 72)

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

    W = sum(weights)
    q, r = divmod(W, N)

    print(f"Total weight = {W}")
    print(f"W = {q}×{N} + {r}")

    print()

    print("Columns")
    print("-------")

    for j in range(N):

        marker = "HIGH" if loads[j] == q + 1 else "LOW "

        print(
            f"{j:2d}: "
            f"{loads[j]:2d}   "
            f"{marker}"
        )

    print()

    print("Histogram")
    print(Counter(loads))

    print()

    print("High columns")

    high = [i for i, x in enumerate(loads) if x == q + 1]

    print(high)

    print()

    print("Prefix vertices")

    print(walk)

    print()

    print("Unique prefix vertices")

    print(sorted(set(walk)))

    print()

    print("Vertex multiplicities")

    c = Counter(walk)

    for i in range(N):
        print(f"{i:2d}: {c[i]}")


if __name__ == "__main__":

    analyse([6,3,5],8)

    print()

    analyse(
        [4,13,13,11,12,5,10,2,0,12],
        15
    )

    print()

    analyse(
        [24,26,28,11,8,20,19,26,2,30],
        31
    )
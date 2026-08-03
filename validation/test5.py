from collections import Counter


def partition(weights):

    P = [0]

    for w in weights:
        P.append(P[-1] + w)

    return P


def residue_histogram(weights, N):

    P = partition(weights)

    hist = Counter()

    for i, w in enumerate(weights):

        for x in range(P[i], P[i+1]):
            hist[x % N] += 1

    return hist


def bcmc_histogram(weights, N):

    loads = [0] * N

    s = 0

    for w in weights:

        for j in range(w):
            loads[(s + j) % N] += 1

        s = (s + w) % N

    return Counter(
        {i: loads[i] for i in range(N)}
    )


def verify(weights, N):

    print("=" * 70)

    print(weights)

    print()

    residues = residue_histogram(weights, N)

    bcmc = bcmc_histogram(weights, N)

    print("Residues")

    for i in range(N):
        print(f"{i:2d}: {residues[i]}")

    print()

    print("BCMC")

    for i in range(N):
        print(f"{i:2d}: {bcmc[i]}")

    print()

    print("Equal?")

    print(residues == bcmc)


verify([6,3,5],8)
from itertools import product


def column_loads(weights, N):

    loads = [0] * N

    offset = 0

    for w in weights:

        for j in range(w):
            loads[(offset + j) % N] += 1

        offset = (offset + w) % N

    return loads


def exhaustive_test(N, C):

    total = 0

    for weights in product(range(N + 1), repeat=C):

        total += 1

        L = column_loads(weights, N)

        imbalance = max(L) - min(L)

        if imbalance > 1:

            print("COUNTEREXAMPLE FOUND")
            print()

            print(f"N = {N}")
            print(f"C = {C}")
            print(f"weights = {list(weights)}")
            print(f"loads = {L}")
            print(f"imbalance = {imbalance}")

            return False

    print(f"Checked {total:,} cases.")
    print("No counterexamples.")

    return True


if __name__ == "__main__":

    for N in range(2,8):

        print()

        print(f"Testing N={N}")

        exhaustive_test(N, N)
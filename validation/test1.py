import random

from collections import Counter


def column_loads(weights, N):

    loads = [0] * N

    s = 0

    for w in weights:

        for j in range(w):
            loads[(s + j) % N] += 1

        s = (s + w) % N

    return loads

def run_random_tests(num_tests=100000):

    worst = 0
    worst_case = None

    for _ in range(num_tests):

        N = random.randint(2, 64)

        # Any number of rows
        C = random.randint(1, 128)

        weights = [random.randint(0, N) for _ in range(C)]

        L = column_loads(weights, N)

        imbalance = max(L) - min(L)

        if imbalance > worst:
            worst = imbalance
            worst_case = (N, C, weights, L)

        if imbalance > 1:
            print("COUNTEREXAMPLE FOUND")
            print(f"N = {N}")
            print(f"C = {C}")
            print(f"weights = {weights}")
            print(f"loads = {L}")
            print(f"imbalance = {imbalance}")
            return

    print(f"No counterexample in {num_tests:,} tests.")
    print(f"Worst imbalance observed = {worst}")

    if worst_case:

        N, C, weights, L = worst_case

        print(worst_case)

        print(Counter(L))


if __name__ == "__main__":
    run_random_tests()

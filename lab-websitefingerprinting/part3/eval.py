import json
import os
import numpy as np

from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report
from sklearn.model_selection import train_test_split


def eval():
    y_pred_full = []
    y_test_full = []

    # Repeat training 10 times to reduce randomness
    for i in range(10):

        # Load traces
        traces_path = os.path.join(os.path.dirname(__file__), "traces.out")

        with open(traces_path, "r") as f:
            data = json.load(f)

        traces = data["traces"]
        # Normalize trace lengths so the classifier gets a rectangular matrix.
        lengths = [len(t) for t in traces]
        fixed_len = max(lengths)
        if len(set(lengths)) != 1:
            print(f"WARNING: variable trace lengths (min={min(lengths)}, max={fixed_len}); padding to {fixed_len}.")
        X = np.array([
            (t[:fixed_len] + [t[-1]] * (fixed_len - len(t))) if len(t) else [0] * fixed_len
            for t in traces
        ])
        y = np.array(data["labels"])

        # Train/test split
        X_train, X_test, y_train, y_test = train_test_split(
            X,
            y,
            test_size=0.2,
            random_state=i,
            stratify=y
        )

        # Train classifier
        clf = RandomForestClassifier(
            n_estimators=200,
            random_state=i
        )

        clf.fit(X_train, y_train)

        # Predict
        y_pred = clf.predict(X_test)

        y_test_full.extend(y_test)
        y_pred_full.extend(y_pred)

    # Print results
    print(classification_report(y_test_full, y_pred_full))


if __name__ == "__main__":
    eval()

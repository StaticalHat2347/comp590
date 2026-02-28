import json
import os
import numpy as np

from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import classification_report
from sklearn.model_selection import train_test_split


def eval():
    y_pred_full, y_test_full = [], []

    # Re-train 10 times in order to reduce effects of randomness
    for i in range(10):
        ### TODO: Exercise 2-5
        ### 1. Load data from traces file
        ### 2. Split data into X_train, X_test, y_train, y_test with train_test_split
        ### 3. Train classifier with X_train and y_train
        ### 4. Use classifier to make predictions on X_test. Save the result to a variable called y_pred
        traces_path = os.path.join(os.path.dirname(__file__), "traces.out")
        with open(traces_path, "r") as f:
            data = json.load(f)

        X = np.array(data["traces"])
        y = np.array(data["labels"])

        X_train, X_test, y_train, y_test = train_test_split(
            X, y, test_size=0.2, random_state=i, stratify=y
        )

        clf = RandomForestClassifier(n_estimators=200, random_state=i)
        clf.fit(X_train, y_train)
        y_pred = clf.predict(X_test)

        # Do not modify the next two lines
        y_test_full.extend(y_test)
        y_pred_full.extend(y_pred)

    ### TODO: Exercise 2-5 (continued)
    ### 5. Print classification report using y_test_full and y_pred_full
    print(classification_report(y_test_full, y_pred_full))

if __name__ == "__main__":
    eval()

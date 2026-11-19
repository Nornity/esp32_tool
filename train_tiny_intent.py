#!/usr/bin/env python3
"""Train the compact intent router that conditions the tiny character generator."""
from __future__ import annotations
from pathlib import Path
import numpy as np
from chat_corpus import DATA
FEATURES = 1024
H1, H2, CLASSES = 192, 96, 17
OUT = Path(__file__).with_name("tiny_chat_intent.h")

def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def features(text: str) -> np.ndarray:
    raw = text.lower().encode("utf-8")
    tokens: list[bytearray] = []
    token = bytearray()
    for b in raw + b" ":
        word = b >= 128 or b == 95 or 48 <= b <= 57 or 65 <= b <= 90 or 97 <= b <= 122
        if word:
            token.append(b + 32 if 65 <= b <= 90 else b)
        elif token:
            tokens.append(token)
            token = bytearray()
    x = np.zeros(FEATURES, dtype=np.float32)
    for index, t in enumerate(tokens):
        x[fnv1a(bytes(t)) & (FEATURES - 1)] = 1.0
        for i in range(len(t) - 2):
            x[fnv1a(t[i:i+3]) & (FEATURES - 1)] = 1.0
        if index:
            x[fnv1a(bytes(tokens[index - 1]) + b" " + bytes(t)) & (FEATURES - 1)] = 1.0
    return x


def train() -> tuple[np.ndarray, ...]:
    # Split original phrases before augmentation; otherwise near-duplicate wrapped
    # phrases leak into validation and make the reported score misleading.
    rng = np.random.default_rng(20261109)
    train_phrases, validation_phrases = [], []
    for label, packed in DATA.items():
        phrases = packed.split("|")
        order = rng.permutation(len(phrases))
        holdout = max(1, round(len(phrases) * 0.20))
        validation_phrases.extend((phrases[i], label) for i in order[:holdout])
        train_phrases.extend((phrases[i], label) for i in order[holdout:])

    examples, labels = [], []
    for phrase, label in train_phrases:
        examples.append(features(phrase)); labels.append(label)
        # Natural wrappers broaden phrasing without contaminating validation.
        for prefix in ("пожалуйста ", "скажи ", "мне нужно "):
            examples.append(features(prefix + phrase)); labels.append(label)
    Xtr = np.asarray(examples, dtype=np.float32)
    ytr = np.asarray(labels, dtype=np.int64)
    Xv = np.asarray([features(p) for p, _ in validation_phrases], dtype=np.float32)
    yv = np.asarray([label for _, label in validation_phrases], dtype=np.int64)

    dims = (FEATURES, H1, H2, CLASSES)
    weights = [rng.normal(0, np.sqrt(2 / (dims[i] + dims[i+1])), (dims[i], dims[i+1])).astype(np.float32) for i in range(3)]
    biases = [np.zeros(dims[i+1], dtype=np.float32) for i in range(3)]
    mw = [np.zeros_like(w) for w in weights]; vw = [np.zeros_like(w) for w in weights]
    mb = [np.zeros_like(b) for b in biases]; vb = [np.zeros_like(b) for b in biases]
    step = 0; lr = 0.0015; batch = 64; epochs = 180; decay = 1e-5
    best_acc = -1.0; best_weights = None; best_epoch = 0
    for epoch in range(epochs):
        order = rng.permutation(len(ytr))
        for start in range(0, len(order), batch):
            ids = order[start:start+batch]; xb, yb = Xtr[ids], ytr[ids]; n = len(ids)
            z1 = xb @ weights[0] + biases[0]; a1 = np.maximum(z1, 0)
            z2 = a1 @ weights[1] + biases[1]; a2 = np.maximum(z2, 0)
            z3 = a2 @ weights[2] + biases[2]; z3 -= z3.max(axis=1, keepdims=True)
            probs = np.exp(z3); probs /= probs.sum(axis=1, keepdims=True)
            probs[np.arange(n), yb] -= 1; probs /= n
            gw2 = a2.T @ probs + decay * weights[2]; gb2 = probs.sum(axis=0)
            ga2 = probs @ weights[2].T; gz2 = ga2 * (z2 > 0)
            gw1 = a1.T @ gz2 + decay * weights[1]; gb1 = gz2.sum(axis=0)
            ga1 = gz2 @ weights[1].T; gz1 = ga1 * (z1 > 0)
            gw0 = xb.T @ gz1 + decay * weights[0]; gb0 = gz1.sum(axis=0)
            gradsW, gradsB = [gw0,gw1,gw2], [gb0,gb1,gb2]
            step += 1
            for i in range(3):
                mw[i] = .9*mw[i] + .1*gradsW[i]; vw[i] = .999*vw[i] + .001*gradsW[i]**2
                mb[i] = .9*mb[i] + .1*gradsB[i]; vb[i] = .999*vb[i] + .001*gradsB[i]**2
                mwHat = mw[i] / (1-.9**step); vwHat = vw[i] / (1-.999**step)
                mbHat = mb[i] / (1-.9**step); vbHat = vb[i] / (1-.999**step)
                weights[i] -= lr * mwHat / (np.sqrt(vwHat)+1e-8)
                biases[i] -= lr * mbHat / (np.sqrt(vbHat)+1e-8)
        if epoch in (0, 9, 19, 39, 59, 79, 119, 179):
            p = Xv
            for layer, (w, b) in enumerate(zip(weights, biases)):
                p = p @ w + b
                if layer < 2: p = np.maximum(p, 0)
            acc = float((p.argmax(axis=1) == yv).mean())
            print(f"epoch {epoch+1:3d}: validation accuracy {acc:.3f}")
            if acc > best_acc:
                best_acc, best_epoch = acc, epoch + 1
                best_weights = ([w.copy() for w in weights], [b.copy() for b in biases])
    weights, biases = best_weights
    print(f"selected epoch {best_epoch}: validation accuracy {best_acc:.3f}")
    return (*weights, *biases, Xv, yv)


def qmatrix(w: np.ndarray) -> tuple[np.ndarray, float]:
    scale = float(np.max(np.abs(w)) / 127.0)
    return np.clip(np.rint(w / scale), -127, 127).astype(np.int8), scale


def c_array(name: str, values: np.ndarray, per_line: int = 24) -> str:
    flat = values.reshape(-1)
    lines = [f"const int8_t {name}[{len(flat)}] = {{"]
    for i in range(0, len(flat), per_line):
        lines.append("  " + ",".join(str(int(x)) for x in flat[i:i+per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def f_array(name: str, values: np.ndarray) -> str:
    body = ",".join(f"{float(x):.8g}f" for x in values)
    return f"const float {name}[{len(values)}] = {{{body}}};"


def main() -> None:
    w0,w1,w2,b0,b1,b2,Xv,yv = train()
    q0,s0=qmatrix(w0); q1,s1=qmatrix(w1); q2,s2=qmatrix(w2)
    p = np.maximum(Xv @ (q0.reshape(FEATURES,H1).astype(np.float32)*s0) + b0, 0)
    p = np.maximum(p @ (q1.reshape(H1,H2).astype(np.float32)*s1) + b1, 0)
    p = p @ (q2.reshape(H2,CLASSES).astype(np.float32)*s2) + b2
    qpred = p.argmax(axis=1)
    qacc = float((qpred == yv).mean())
    margins = np.sort(p, axis=1)[:, -1] - np.sort(p, axis=1)[:, -2]
    accepted = margins >= 0.12
    covered = int(accepted.sum())
    accepted_acc = float((qpred[accepted] == yv[accepted]).mean()) if covered else 0.0
    print(f"quantized validation accuracy: {qacc:.3f} ({int((qpred == yv).sum())}/{len(yv)}); margin-gated coverage {covered}/{len(yv)}, accuracy {accepted_acc:.3f}")
    for label in range(CLASSES):
        mask = yv == label
        print(f"  class {label:2d}: {(qpred[mask] == yv[mask]).sum()}/{mask.sum()}")
    text = ["#pragma once", "#include <stdint.h>", f"#define CHAT_FEATURES {FEATURES}", f"#define CHAT_H1 {H1}", f"#define CHAT_H2 {H2}", f"#define CHAT_CLASSES {CLASSES}", 
            f"const float chatScale1 = {s0:.9g}f;", f"const float chatScale2 = {s1:.9g}f;", f"const float chatScale3 = {s2:.9g}f;",
            c_array("chatW1Q",q0), c_array("chatW2Q",q1), c_array("chatW3Q",q2), f_array("chatB1",b0), f_array("chatB2",b1), f_array("chatB3",b2)]
    OUT.write_text("\n\n".join(text)+"\n", encoding="utf-8")
    print(f"wrote {OUT.name}: {q0.nbytes+q1.nbytes+q2.nbytes:,} quantized weight bytes")

if __name__ == "__main__":
    main()

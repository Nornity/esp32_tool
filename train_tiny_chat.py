#!/usr/bin/env python3
"""Train a tiny prompt-conditioned character RNN for the ESP32-C3 chat."""
from __future__ import annotations

from pathlib import Path
import numpy as np
from chat_corpus import DATA

HIDDEN = 96
EPOCHS = 120
BATCH = 64
SEED = 20261109
OUT = Path(__file__).with_name("tiny_chat_model.h")
CLASS_TAGS = ["приветствие", "батарея", "время", "обзор_wifi", "каналы_wifi", "ик", "игры", "энергосбережение", "устройство", "диагностика_сети", "макрос", "сохраненные_сети", "помощь", "благодарность", "шутка", "прощание", "вне_тем"]

# The network learns to continue a prompt/answer stream; these are training targets,
# not a runtime lookup table. Text is generated one Unicode character at a time.
ANSWERS = {
    0: "Привет! Я работаю прямо на ESP32 и отвечаю локально.",
    1: "Проверь заряд батареи в меню Tools → Battery. Я могу отвечать только по тем данным, которые реально доступны устройству.",
    2: "Время можно посмотреть в Tools → Clock. Для точной синхронизации нужны сохранённая Wi-Fi сеть и доступ к интернету.",
    3: "Для пассивного обзора сетей открой WiFi → Analyze → Signal. Там видны сети и уровень RSSI.",
    4: "В WiFi → Channels есть оценка загруженности каналов 2.4 ГГц по видимым точкам доступа.",
    5: "ИК-команда отправляется из Proj или TV. Выбери устройство и кнопку пульта.",
    6: "Открой Games в главном меню: там есть несколько мини-игр для устройства.",
    7: "Экран выключается после бездействия для экономии батареи. Нажми любую кнопку, чтобы разбудить его.",
    8: "Это Tricorder на ESP32-C3 с OLED, кнопками, Wi-Fi и ИК-передатчиком.",
    9: "Сетевые тесты запускаются отдельно и требуют разрешённой сети. Чат сам их не выполняет.",
    10: "ИК-макросы находятся в Tools → Macro. Избранные команды — в Tools → IR Fav.",
    11: "Сохранёнными Wi-Fi сетями управляй в меню WiFi. Я не раскрываю сохранённые пароли.",
    12: "Я могу рассказать о зарядке, времени, Wi-Fi, каналах, ИК, играх и меню устройства.",
    13: "Пожалуйста! Рад, что смог помочь.",
    14: "Почему ESP32 не спорит с батареей? Потому что та всегда на последнем проценте!",
    15: "Пока! Чат работает локально на устройстве.",
    16: "Я пока не умею отвечать на такие вопросы. Мои знания ограничены функциями Tricorder.",
}


def fnv_quantize(w: np.ndarray) -> tuple[np.ndarray, float]:
    scale = max(float(np.max(np.abs(w))) / 127.0, 1e-8)
    return np.clip(np.rint(w / scale), -127, 127).astype(np.int8), scale


def build_examples():
    rng = np.random.default_rng(SEED)
    train, valid = [], []
    for label, packed in DATA.items():
        phrases = packed.split("|")
        order = rng.permutation(len(phrases))
        nval = max(1, round(len(phrases) * .20))
        for index in order:
            prompt = phrases[index]
            prefix = f"Вопрос: {prompt}\nТема: {CLASS_TAGS[label]}\nОтвет: "
            ids = prefix + ANSWERS[label] + "\n"
            example = (ids, len(prefix))
            (valid if index in order[:nval] else train).append(example)
    return train, valid


def train_model():
    rng = np.random.default_rng(SEED)
    train, valid = build_examples()
    all_text = "".join(seq for seq, _ in train + valid) + "?"
    chars = sorted(set(all_text))
    char_to_id = {c: i for i, c in enumerate(chars)}
    sequences = []
    for seq, start in train + valid:
        sequences.append(([char_to_id[c] for c in seq], start))
    train_seq, valid_seq = sequences[:len(train)], sequences[len(train):]
    vocab = len(chars)
    dimensions = (vocab, HIDDEN, vocab)
    Wxh = rng.normal(0, np.sqrt(1 / HIDDEN), (vocab, HIDDEN)).astype(np.float32)
    Whh = rng.normal(0, np.sqrt(1 / HIDDEN), (HIDDEN, HIDDEN)).astype(np.float32)
    Why = rng.normal(0, np.sqrt(1 / HIDDEN), (HIDDEN, vocab)).astype(np.float32)
    bh = np.zeros(HIDDEN, np.float32); by = np.zeros(vocab, np.float32)
    params = [Wxh, Whh, Why, bh, by]
    m = [np.zeros_like(x) for x in params]; v = [np.zeros_like(x) for x in params]
    step = 0; best_loss = float("inf"); best = None

    def evaluate(dataset):
        total_loss = 0.0; total_chars = 0
        for start in range(0, len(dataset), BATCH):
            batch = dataset[start:start+BATCH]
            lens = np.array([len(s)-1 for s, _ in batch], dtype=np.int32)
            T = int(lens.max())
            ids = np.zeros((T+1, len(batch)), dtype=np.int32)
            for j, (seq, _) in enumerate(batch): ids[:len(seq), j] = seq
            h = np.zeros((len(batch), HIDDEN), np.float32)
            for t in range(T):
                active = (t < lens)[:, None]
                hn = np.tanh(Wxh[ids[t]] + h @ Whh + bh)
                h = np.where(active, hn, h)
                logits = h @ Why + by
                logits -= logits.max(axis=1, keepdims=True)
                p = np.exp(logits); p /= p.sum(axis=1, keepdims=True)
                for j, (_, response_start) in enumerate(batch):
                    if t + 1 >= response_start and t < lens[j]:
                        total_loss -= float(np.log(max(float(p[j, ids[t+1, j]]), 1e-9)))
                        total_chars += 1
        return total_loss / max(total_chars, 1)

    for epoch in range(EPOCHS):
        order = rng.permutation(len(train_seq))
        for offset in range(0, len(order), BATCH):
            batch = [train_seq[i] for i in order[offset:offset+BATCH]]
            n = len(batch); lens = np.array([len(s)-1 for s, _ in batch], np.int32); T = int(lens.max())
            ids = np.zeros((T+1, n), np.int32)
            response_starts = np.array([rs for _, rs in batch], np.int32)
            for j, (seq, _) in enumerate(batch): ids[:len(seq), j] = seq
            hs = [np.zeros((n, HIDDEN), np.float32)]
            probs = []
            loss_mask = []
            for t in range(T):
                active = (t < lens)[:, None]
                hn = np.tanh(Wxh[ids[t]] + hs[-1] @ Whh + bh)
                hs.append(np.where(active, hn, hs[-1]))
                logits = hs[-1] @ Why + by
                logits -= logits.max(axis=1, keepdims=True)
                p = np.exp(logits); p /= p.sum(axis=1, keepdims=True)
                probs.append(p)
                loss_mask.append(((t + 1 >= response_starts) & (t < lens)).astype(np.float32))

            grads = [np.zeros_like(x) for x in params]
            dWxh, dWhh, dWhy, dbh, dby = grads
            dhnext = np.zeros((n, HIDDEN), np.float32)
            for t in range(T-1, -1, -1):
                mask = loss_mask[t]
                dy = probs[t].copy()
                dy[np.arange(n), ids[t+1]] -= 1.0
                dy *= mask[:, None] / n
                dWhy += hs[t+1].T @ dy; dby += dy.sum(axis=0)
                dh = dy @ Why.T + dhnext
                da = dh * (1.0 - hs[t+1] * hs[t+1])
                da *= (t < lens)[:, None]
                np.add.at(dWxh, ids[t], da)
                dWhh += hs[t].T @ da; dbh += da.sum(axis=0)
                dhnext = da @ Whh.T + dh * (1.0 - (t < lens)[:, None])
            norm = np.sqrt(sum(float(np.sum(g*g)) for g in grads))
            if norm > 5.0:
                grads = [g * (5.0 / norm) for g in grads]
            step += 1
            lr = 0.0018
            for k, (param, grad) in enumerate(zip(params, grads)):
                m[k] = .9*m[k] + .1*grad; v[k] = .999*v[k] + .001*grad*grad
                mh = m[k]/(1-.9**step); vh = v[k]/(1-.999**step)
                param -= lr * mh / (np.sqrt(vh) + 1e-8)
        if epoch % 5 == 0 or epoch == EPOCHS-1:
            vl = evaluate(valid_seq)
            print(f"epoch {epoch+1:2d}: held-out response char loss {vl:.3f}")
            if vl < best_loss:
                best_loss = vl; best = [x.copy() for x in params]
    Wxh, Whh, Why, bh, by = best
    print(f"selected model held-out char loss: {best_loss:.3f}; vocabulary {vocab}, hidden {HIDDEN}")
    return chars, Wxh, Whh, Why, bh, by


def array(name: str, x: np.ndarray, typ: str = "int8_t", per_line: int = 24) -> str:
    flat = x.reshape(-1)
    rows = [f"const {typ} {name}[{len(flat)}] = {{"]
    for i in range(0, len(flat), per_line): rows.append("  " + ",".join(str(int(a)) for a in flat[i:i+per_line]) + ",")
    rows.append("};")
    return "\n".join(rows)


def float_array(name: str, x: np.ndarray) -> str:
    return f"const float {name}[{len(x)}] = {{" + ",".join(f"{float(a):.8g}f" for a in x) + "};"


def main():
    chars, Wxh, Whh, Why, bh, by = train_model()
    qx,sx = fnv_quantize(Wxh); qh,sh = fnv_quantize(Whh); qy,sy = fnv_quantize(Why)
    text = ["#pragma once", "#include <stdint.h>", f"#define CHAT_VOCAB {len(chars)}", f"#define CHAT_HIDDEN {HIDDEN}",
            f"const float chatScaleX = {sx:.9g}f;", f"const float chatScaleH = {sh:.9g}f;", f"const float chatScaleY = {sy:.9g}f;",
            array("chatChars", np.array([ord(c) for c in chars], dtype=np.uint16), "uint16_t"),
            array("chatWxhQ", qx), array("chatWhhQ", qh), array("chatWhyQ", qy),
            float_array("chatBh", bh), float_array("chatBy", by)]
    OUT.write_text("\n\n".join(text) + "\n", encoding="utf-8")
    print(f"wrote {OUT.name}: {(qx.size+qh.size+qy.size):,} quantized weight bytes")


if __name__ == "__main__":
    main()

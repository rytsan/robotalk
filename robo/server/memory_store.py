#!/usr/bin/env python3
"""Memória persistente híbrida para o servidor do robô.

Esta primeira versão usa SQLite puro para manter o sistema leve no Raspberry:
- `conversation_turns`: histórico persistente de conversa;
- `memories`: fatos/assertivas com forma de grafo e caminho de árvore;
- `memory_links`: arestas explícitas entre memórias.

O objetivo é responder fatos simples sem chamar LLM e fornecer contexto curto
para a LLM quando ela for necessária.
"""

from __future__ import annotations

import os
import re
import hashlib
import json
import math
import sqlite3
import time
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from urllib import error, request

EMBEDDING_DIMS = 128
SEMANTIC_RESPONSE_THRESHOLD = 0.88

# Embedding neural via Ollama, com o hashing local como rede de segurança.
# Vazio em ROBO_EMBED_MODEL = usar só o hashing.
EMBED_MODEL = os.environ.get("ROBO_EMBED_MODEL", "nomic-embed-text").strip()
EMBED_URL = os.environ.get("ROBO_EMBED_URL", "http://127.0.0.1:11434/api/embeddings").strip()
EMBED_TIMEOUT_S = float(os.environ.get("ROBO_EMBED_TIMEOUT_S", "20"))
HASH_MODEL_NAME = "hash-128"
EMBED_CACHE_MAX = 256

# Vetores de modelos diferentes não são comparáveis: 128 dims contra 768 daria
# cosseno 0 e a busca degradaria em silêncio. Por isso cada linha guarda de que
# modelo veio, e o modelo ativo é decidido uma vez por sessão.
_active_embed_model: str | None = None
_embed_cache: dict[str, list[float]] = {}


def normalize_text(text: str) -> str:
    decomposed = unicodedata.normalize("NFD", text.lower().strip())
    without_accents = "".join(ch for ch in decomposed if unicodedata.category(ch) != "Mn")
    return re.sub(r"\s+", " ", without_accents)


def now_ts() -> float:
    return time.time()


def tokenize(text: str) -> list[str]:
    return [word for word in re.findall(r"[a-z0-9_]+", normalize_text(text)) if len(word) >= 2]


def stable_hash(value: str) -> int:
    return int.from_bytes(hashlib.blake2b(value.encode("utf-8"), digest_size=8).digest(), "big")


def text_embedding(text: str, dims: int = EMBEDDING_DIMS) -> list[float]:
    """Gera um embedding local leve por hashing.

    Isto não substitui um embedding neural, mas dá uma métrica útil de
    proximidade lexical/semântica sem depender de rede nem de pacotes grandes.
    """

    vector = [0.0] * dims
    tokens = tokenize(text)
    features: list[tuple[str, float]] = []

    for token in tokens:
        features.append((f"w:{token}", 1.0))
        if len(token) >= 4:
            for index in range(len(token) - 2):
                features.append((f"c:{token[index:index + 3]}", 0.35))

    for left, right in zip(tokens, tokens[1:]):
        features.append((f"b:{left}_{right}", 0.8))

    if not features:
        return vector

    for feature, weight in features:
        hashed = stable_hash(feature)
        slot = hashed % dims
        sign = 1.0 if ((hashed >> 8) & 1) else -1.0
        vector[slot] += sign * weight

    norm = math.sqrt(sum(value * value for value in vector))
    if norm == 0:
        return vector
    return [value / norm for value in vector]


def embed_via_ollama(text: str) -> list[float] | None:
    """Pede o embedding ao Ollama. Devolve None em qualquer falha."""
    if not EMBED_MODEL or not EMBED_URL:
        return None

    payload = json.dumps({"model": EMBED_MODEL, "prompt": text}).encode("utf-8")
    req = request.Request(
        EMBED_URL,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with request.urlopen(req, timeout=EMBED_TIMEOUT_S) as response:
            data = json.loads(response.read().decode("utf-8"))
    except (error.URLError, OSError, json.JSONDecodeError):
        return None

    vector = data.get("embedding")
    if not isinstance(vector, list) or not vector:
        return None

    values = [float(v) for v in vector]
    norm = math.sqrt(sum(v * v for v in values))
    if norm == 0:
        return None
    return [v / norm for v in values]


def resolve_embed_model() -> str:
    """Escolhe o modelo de embedding uma vez por sessão.

    Decidir a cada chamada permitiria misturar vetores de modelos diferentes na
    mesma tabela, e aí nada mais seria comparável com nada.
    """
    global _active_embed_model
    if _active_embed_model is not None:
        return _active_embed_model

    if EMBED_MODEL and embed_via_ollama("teste") is not None:
        _active_embed_model = EMBED_MODEL
        print(f"Embedding: '{EMBED_MODEL}' via Ollama.")
    else:
        _active_embed_model = HASH_MODEL_NAME
        if EMBED_MODEL:
            print(f"Embedding: '{EMBED_MODEL}' indisponível; usando {HASH_MODEL_NAME}.")
            print("           A busca por significado fica fraca até o Ollama voltar.")
        else:
            print(f"Embedding: {HASH_MODEL_NAME} por configuração.")

    return _active_embed_model


def embedding_for(text: str) -> tuple[list[float], str]:
    """Devolve (vetor, modelo). Cai no hashing se o Ollama falhar na hora."""
    modelo = resolve_embed_model()

    if modelo == HASH_MODEL_NAME:
        return text_embedding(text), HASH_MODEL_NAME

    chave = text.strip()
    if chave in _embed_cache:
        return _embed_cache[chave], modelo

    vector = embed_via_ollama(chave)
    if vector is None:
        # Falha pontual: não troca o modelo da sessão, só devolve algo usável.
        # A linha fica marcada como hash e é regenerada no próximo arranque.
        return text_embedding(text), HASH_MODEL_NAME

    # A mesma fala é embutida várias vezes por turno (turno, cache, busca).
    if len(_embed_cache) >= EMBED_CACHE_MAX:
        _embed_cache.clear()
    _embed_cache[chave] = vector
    return vector, modelo


def embedding_to_json(vector: list[float]) -> str:
    return json.dumps(vector, separators=(",", ":"))


def embedding_from_json(raw: str | None) -> list[float]:
    if not raw:
        return []
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return []
    if not isinstance(data, list):
        return []
    return [float(value) for value in data]


def cosine_similarity(left: list[float], right: list[float]) -> float:
    if not left or not right or len(left) != len(right):
        return 0.0
    return sum(a * b for a, b in zip(left, right))


def fts_query(text: str) -> str:
    words = [word for word in tokenize(text) if len(word) >= 3]
    return " OR ".join(f"{word}*" for word in words[:10])


@dataclass(frozen=True)
class MemoryFact:
    subject: str
    predicate: str
    object_value: str
    tree_path: str
    confidence: float = 0.8
    source: str = "conversation"


@dataclass(frozen=True)
class SearchResult:
    row: sqlite3.Row
    score: float
    lexical_score: float
    embedding_score: float


class MemoryStore:
    def __init__(self, db_path: Path) -> None:
        self.db_path = db_path
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(str(db_path), check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.fts_enabled = True
        self.init_schema()

    def init_schema(self) -> None:
        self.conn.executescript(
            """
            PRAGMA journal_mode=WAL;

            CREATE TABLE IF NOT EXISTS conversation_turns (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at REAL NOT NULL,
                speaker_id TEXT NOT NULL,
                role TEXT NOT NULL,
                content TEXT NOT NULL,
                normalized_content TEXT NOT NULL,
                embedding TEXT
            );

            CREATE TABLE IF NOT EXISTS memories (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at REAL NOT NULL,
                updated_at REAL NOT NULL,
                subject TEXT NOT NULL,
                predicate TEXT NOT NULL,
                object_value TEXT NOT NULL,
                tree_path TEXT NOT NULL,
                confidence REAL NOT NULL,
                source TEXT NOT NULL,
                embedding TEXT,
                use_count INTEGER NOT NULL DEFAULT 0,
                last_used_at REAL,
                UNIQUE(subject, predicate)
            );

            CREATE INDEX IF NOT EXISTS idx_memories_subject
                ON memories(subject);
            CREATE INDEX IF NOT EXISTS idx_memories_subject_predicate
                ON memories(subject, predicate);
            CREATE INDEX IF NOT EXISTS idx_memories_tree_path
                ON memories(tree_path);
            CREATE INDEX IF NOT EXISTS idx_memories_object
                ON memories(object_value);
            CREATE INDEX IF NOT EXISTS idx_turns_created_at
                ON conversation_turns(created_at);

            CREATE TABLE IF NOT EXISTS memory_links (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at REAL NOT NULL,
                from_memory_id INTEGER NOT NULL,
                relation TEXT NOT NULL,
                to_memory_id INTEGER NOT NULL,
                weight REAL NOT NULL DEFAULT 1.0,
                UNIQUE(from_memory_id, relation, to_memory_id),
                FOREIGN KEY(from_memory_id) REFERENCES memories(id),
                FOREIGN KEY(to_memory_id) REFERENCES memories(id)
            );

            CREATE TABLE IF NOT EXISTS response_cache (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at REAL NOT NULL,
                updated_at REAL NOT NULL,
                speaker_id TEXT NOT NULL,
                question TEXT NOT NULL,
                normalized_question TEXT NOT NULL,
                answer TEXT NOT NULL,
                embedding TEXT NOT NULL,
                confidence REAL NOT NULL,
                use_count INTEGER NOT NULL DEFAULT 0,
                last_used_at REAL
            );

            CREATE INDEX IF NOT EXISTS idx_response_cache_question
                ON response_cache(speaker_id, normalized_question);
            CREATE INDEX IF NOT EXISTS idx_response_cache_recent
                ON response_cache(speaker_id, updated_at);
            """
        )
        self.ensure_column("conversation_turns", "embedding", "TEXT")
        self.ensure_column("memories", "embedding", "TEXT")
        self.ensure_column("conversation_turns", "embedding_model", "TEXT")
        self.ensure_column("memories", "embedding_model", "TEXT")
        self.ensure_column("response_cache", "embedding_model", "TEXT")
        self.init_fts()
        self.backfill_indexes()
        self.conn.commit()

    def ensure_column(self, table: str, column: str, definition: str) -> None:
        columns = {row["name"] for row in self.conn.execute(f"PRAGMA table_info({table})")}
        if column not in columns:
            self.conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {definition}")

    def init_fts(self) -> None:
        try:
            self.conn.executescript(
                """
                CREATE VIRTUAL TABLE IF NOT EXISTS conversation_turns_fts
                    USING fts5(content, speaker_id, role);
                CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts
                    USING fts5(subject, predicate, object_value, tree_path);
                CREATE VIRTUAL TABLE IF NOT EXISTS response_cache_fts
                    USING fts5(question, answer, speaker_id);
                """
            )
        except sqlite3.OperationalError as exc:
            self.fts_enabled = False
            print(f"FTS5 indisponível; usando busca vetorial/LIKE: {exc}")

    def fts_replace(self, table: str, rowid: int, values: tuple[str, ...]) -> None:
        if not self.fts_enabled:
            return
        placeholders = ", ".join(["?"] * len(values))
        try:
            self.conn.execute(
                f"INSERT OR REPLACE INTO {table}(rowid, {self.fts_columns(table)}) VALUES (?, {placeholders})",
                (rowid, *values),
            )
        except sqlite3.OperationalError as exc:
            self.fts_enabled = False
            print(f"FTS5 falhou; desabilitando busca lexical: {exc}")

    def fts_columns(self, table: str) -> str:
        if table == "conversation_turns_fts":
            return "content, speaker_id, role"
        if table == "memories_fts":
            return "subject, predicate, object_value, tree_path"
        if table == "response_cache_fts":
            return "question, answer, speaker_id"
        raise ValueError(f"Tabela FTS desconhecida: {table}")

    def precisa_reembutir(self, row: sqlite3.Row, modelo: str) -> bool:
        try:
            atual = row["embedding_model"]
        except (IndexError, KeyError):
            atual = None
        return (not row["embedding"]) or (atual != modelo)

    def backfill_indexes(self) -> None:
        """Reconstrói índices lexicais e regenera embeddings desatualizados.

        Trocar o modelo de embedding invalida todos os vetores gravados, porque
        vetores de modelos diferentes não são comparáveis. A regeneração é feita
        aqui, uma vez, no arranque.
        """
        modelo = resolve_embed_model()
        regerados = 0

        turns = self.conn.execute(
            """
            SELECT id, content, speaker_id, role, embedding, embedding_model
            FROM conversation_turns
            """
        ).fetchall()
        for row in turns:
            if self.precisa_reembutir(row, modelo):
                vector, usado = embedding_for(row["content"])
                self.conn.execute(
                    "UPDATE conversation_turns SET embedding = ?, embedding_model = ? WHERE id = ?",
                    (embedding_to_json(vector), usado, int(row["id"])),
                )
                regerados += 1
            self.fts_replace(
                "conversation_turns_fts",
                int(row["id"]),
                (row["content"], row["speaker_id"], row["role"]),
            )

        memories = self.conn.execute(
            """
            SELECT id, subject, predicate, object_value, tree_path, embedding, embedding_model
            FROM memories
            """
        ).fetchall()
        for row in memories:
            embedding_text = f"{row['subject']} {row['predicate']} {row['object_value']} {row['tree_path']}"
            if self.precisa_reembutir(row, modelo):
                vector, usado = embedding_for(embedding_text)
                self.conn.execute(
                    "UPDATE memories SET embedding = ?, embedding_model = ? WHERE id = ?",
                    (embedding_to_json(vector), usado, int(row["id"])),
                )
                regerados += 1
            self.fts_replace(
                "memories_fts",
                int(row["id"]),
                (row["subject"], row["predicate"], row["object_value"], row["tree_path"]),
            )

        caches = self.conn.execute(
            """
            SELECT id, question, answer, speaker_id, embedding, embedding_model
            FROM response_cache
            """
        ).fetchall()
        for row in caches:
            if self.precisa_reembutir(row, modelo):
                vector, usado = embedding_for(row["question"])
                self.conn.execute(
                    "UPDATE response_cache SET embedding = ?, embedding_model = ? WHERE id = ?",
                    (embedding_to_json(vector), usado, int(row["id"])),
                )
                regerados += 1
            self.fts_replace(
                "response_cache_fts",
                int(row["id"]),
                (row["question"], row["answer"], row["speaker_id"]),
            )

        if regerados:
            print(f"Memória: {regerados} embedding(s) regenerado(s) para '{modelo}'.")

    def add_turn(self, role: str, content: str, speaker_id: str = "ricardo") -> int | None:
        content = content.strip()
        if not content:
            return None
        vector, modelo = embedding_for(content)
        cursor = self.conn.execute(
            """
            INSERT INTO conversation_turns
                (created_at, speaker_id, role, content, normalized_content, embedding, embedding_model)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            """,
            (
                now_ts(),
                speaker_id,
                role,
                content,
                normalize_text(content),
                embedding_to_json(vector),
                modelo,
            ),
        )
        rowid = int(cursor.lastrowid)
        self.fts_replace("conversation_turns_fts", rowid, (content, speaker_id, role))
        self.conn.commit()
        return rowid

    def upsert_fact(self, fact: MemoryFact) -> None:
        now = now_ts()
        subject = normalize_text(fact.subject)
        predicate = normalize_text(fact.predicate)
        object_value = fact.object_value.strip()
        tree_path = fact.tree_path.strip()
        embedding_text = f"{subject} {predicate} {object_value} {tree_path}"
        vector, modelo = embedding_for(embedding_text)
        self.conn.execute(
            """
            INSERT INTO memories
                (created_at, updated_at, subject, predicate, object_value,
                 tree_path, confidence, source, embedding, embedding_model)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(subject, predicate) DO UPDATE SET
                updated_at = excluded.updated_at,
                object_value = excluded.object_value,
                tree_path = excluded.tree_path,
                confidence = max(memories.confidence, excluded.confidence),
                source = excluded.source,
                embedding = excluded.embedding,
                embedding_model = excluded.embedding_model
            """,
            (
                now,
                now,
                subject,
                predicate,
                object_value,
                tree_path,
                fact.confidence,
                fact.source,
                embedding_to_json(vector),
                modelo,
            ),
        )
        row = self.get_fact(subject, predicate, mark_used=False)
        if row is not None:
            self.fts_replace(
                "memories_fts",
                int(row["id"]),
                (subject, predicate, object_value, tree_path),
            )
        self.conn.commit()

    def get_fact(self, subject: str, predicate: str, mark_used: bool = True) -> sqlite3.Row | None:
        row = self.conn.execute(
            """
            SELECT * FROM memories
            WHERE subject = ? AND predicate = ?
            ORDER BY confidence DESC, updated_at DESC
            LIMIT 1
            """,
            (normalize_text(subject), normalize_text(predicate)),
        ).fetchone()
        if row is not None and mark_used:
            self.mark_used(int(row["id"]))
        return row

    def add_response_cache(
        self,
        question: str,
        answer: str,
        speaker_id: str = "ricardo",
        confidence: float = 0.72,
    ) -> None:
        question = question.strip()
        answer = answer.strip()
        if not question or not answer:
            return
        normalized_question = normalize_text(question)
        existing = self.conn.execute(
            """
            SELECT id FROM response_cache
            WHERE speaker_id = ? AND normalized_question = ?
            LIMIT 1
            """,
            (speaker_id, normalized_question),
        ).fetchone()
        now = now_ts()
        vector, modelo = embedding_for(question)
        embedding = embedding_to_json(vector)
        if existing is None:
            cursor = self.conn.execute(
                """
                INSERT INTO response_cache
                    (created_at, updated_at, speaker_id, question, normalized_question,
                     answer, embedding, confidence, embedding_model)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (now, now, speaker_id, question, normalized_question, answer, embedding, confidence, modelo),
            )
            rowid = int(cursor.lastrowid)
        else:
            rowid = int(existing["id"])
            self.conn.execute(
                """
                UPDATE response_cache
                SET updated_at = ?, question = ?, answer = ?, embedding = ?,
                    confidence = max(confidence, ?), embedding_model = ?
                WHERE id = ?
                """,
                (now, question, answer, embedding, confidence, modelo, rowid),
            )
        self.fts_replace("response_cache_fts", rowid, (question, answer, speaker_id))
        self.conn.commit()

    def semantic_cached_response(
        self,
        text: str,
        speaker_id: str = "ricardo",
        threshold: float = SEMANTIC_RESPONSE_THRESHOLD,
    ) -> tuple[str, float] | None:
        query_vector, query_model = embedding_for(text)
        lexical_ids: dict[int, float] = {}
        query = fts_query(text)
        if self.fts_enabled and query:
            try:
                rows = self.conn.execute(
                    """
                    SELECT rowid, bm25(response_cache_fts) AS rank
                    FROM response_cache_fts
                    WHERE response_cache_fts MATCH ?
                    LIMIT 12
                    """,
                    (query,),
                ).fetchall()
                for index, row in enumerate(rows):
                    lexical_ids[int(row["rowid"])] = max(0.0, 1.0 - (index * 0.06))
            except sqlite3.OperationalError:
                lexical_ids = {}

        rows = self.conn.execute(
            """
            SELECT *
            FROM response_cache
            WHERE speaker_id = ?
            ORDER BY updated_at DESC
            LIMIT 200
            """,
            (speaker_id,),
        ).fetchall()

        best_row: sqlite3.Row | None = None
        best_score = 0.0
        for row in rows:
            # Vetor de outro modelo não é comparável; contar como 0 daria um
            # score baixo enganoso em vez de simplesmente ignorar a linha.
            if row["embedding_model"] != query_model:
                continue
            embedding_score = cosine_similarity(query_vector, embedding_from_json(row["embedding"]))
            lexical_score = lexical_ids.get(int(row["id"]), 0.0)
            confidence = float(row["confidence"])
            score = (embedding_score * 0.68) + (lexical_score * 0.18) + (confidence * 0.14)
            if score > best_score:
                best_score = score
                best_row = row

        if best_row is None or best_score < threshold:
            return None

        self.conn.execute(
            """
            UPDATE response_cache
            SET use_count = use_count + 1, last_used_at = ?
            WHERE id = ?
            """,
            (now_ts(), int(best_row["id"])),
        )
        self.conn.commit()
        return str(best_row["answer"]), best_score

    def facts_by_predicate_prefix(
        self,
        subject: str,
        predicate_prefix: str,
        limit: int = 8,
    ) -> list[sqlite3.Row]:
        rows = self.conn.execute(
            """
            SELECT *
            FROM memories
            WHERE subject = ? AND predicate LIKE ?
            ORDER BY confidence DESC, updated_at DESC
            LIMIT ?
            """,
            (normalize_text(subject), f"{normalize_text(predicate_prefix)}%", limit),
        ).fetchall()
        for row in rows:
            self.mark_used(int(row["id"]))
        return rows

    def mark_used(self, memory_id: int) -> None:
        self.conn.execute(
            """
            UPDATE memories
            SET use_count = use_count + 1, last_used_at = ?
            WHERE id = ?
            """,
            (now_ts(), memory_id),
        )
        self.conn.commit()

    def recent_turns(self, limit: int = 8) -> list[sqlite3.Row]:
        rows = self.conn.execute(
            """
            SELECT role, content
            FROM conversation_turns
            ORDER BY id DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
        return list(reversed(rows))

    def relevant_memories(self, text: str, limit: int = 6) -> list[sqlite3.Row]:
        return [result.row for result in self.hybrid_search_memories(text, limit=limit)]

    def hybrid_search_memories(self, text: str, limit: int = 6) -> list[SearchResult]:
        query_vector, query_model = embedding_for(text)
        lexical_scores: dict[int, float] = {}
        query = fts_query(text)
        if self.fts_enabled and query:
            try:
                rows = self.conn.execute(
                    """
                    SELECT rowid, bm25(memories_fts) AS rank
                    FROM memories_fts
                    WHERE memories_fts MATCH ?
                    LIMIT 24
                    """,
                    (query,),
                ).fetchall()
                for index, row in enumerate(rows):
                    lexical_scores[int(row["rowid"])] = max(0.0, 1.0 - (index * 0.04))
            except sqlite3.OperationalError:
                lexical_scores = {}

        candidate_ids = set(lexical_scores)
        recent_rows = self.conn.execute(
            """
            SELECT id FROM memories
            ORDER BY updated_at DESC
            LIMIT 80
            """
        ).fetchall()
        candidate_ids.update(int(row["id"]) for row in recent_rows)

        if not candidate_ids:
            return self.like_relevant_memories(text, limit)

        placeholders = ", ".join(["?"] * len(candidate_ids))
        rows = self.conn.execute(
            f"SELECT * FROM memories WHERE id IN ({placeholders})",
            tuple(candidate_ids),
        ).fetchall()

        results: list[SearchResult] = []
        for row in rows:
            lexical_score = lexical_scores.get(int(row["id"]), 0.0)
            # Só compara vetores do mesmo modelo; o sinal lexical continua valendo.
            mesmo_modelo = row["embedding_model"] == query_model
            embedding_score = (
                cosine_similarity(query_vector, embedding_from_json(row["embedding"]))
                if mesmo_modelo
                else 0.0
            )
            confidence = float(row["confidence"])
            use_bonus = min(float(row["use_count"]) * 0.015, 0.09)
            has_match_signal = lexical_score > 0.0 or embedding_score >= 0.22
            score = (lexical_score * 0.35) + (embedding_score * 0.45) + (confidence * 0.15) + use_bonus
            if has_match_signal and score > 0.2:
                results.append(SearchResult(row=row, score=score, lexical_score=lexical_score, embedding_score=embedding_score))

        results.sort(key=lambda result: result.score, reverse=True)
        return results[:limit]

    def like_relevant_memories(self, text: str, limit: int = 6) -> list[SearchResult]:
        words = [
            word
            for word in re.findall(r"[a-z0-9_]+", normalize_text(text))
            if len(word) >= 4
        ]
        if not words:
            return []

        clauses = []
        params: list[str | int] = []
        for word in words[:8]:
            like = f"%{word}%"
            clauses.append("(subject LIKE ? OR predicate LIKE ? OR object_value LIKE ? OR tree_path LIKE ?)")
            params.extend([like, like, like, like])
        params.append(limit)

        rows = self.conn.execute(
            f"""
            SELECT *
            FROM memories
            WHERE {" OR ".join(clauses)}
            ORDER BY use_count DESC, confidence DESC, updated_at DESC
            LIMIT ?
            """,
            params,
        ).fetchall()
        return [
            SearchResult(row=row, score=float(row["confidence"]), lexical_score=0.0, embedding_score=0.0)
            for row in rows
        ]

    def close(self) -> None:
        self.conn.close()


IDENTITY_PATTERNS = [
    (
        re.compile(r"\bmeu nome (?:e|é)\s+([a-zA-ZÀ-ÿ][a-zA-ZÀ-ÿ ]{1,40})", re.IGNORECASE),
        "nome",
        "/identidade/nome",
    ),
    (
        re.compile(r"\beu me chamo\s+([a-zA-ZÀ-ÿ][a-zA-ZÀ-ÿ ]{1,40})", re.IGNORECASE),
        "nome",
        "/identidade/nome",
    ),
    (
        re.compile(r"\bpode me chamar de\s+([a-zA-ZÀ-ÿ][a-zA-ZÀ-ÿ ]{1,40})", re.IGNORECASE),
        "apelido",
        "/identidade/apelido",
    ),
]


LIKE_PATTERNS = [
    re.compile(r"\beu gosto de\s+(.{2,80})", re.IGNORECASE),
    re.compile(r"\beu curto\s+(.{2,80})", re.IGNORECASE),
]


def clean_fact_value(value: str) -> str:
    value = re.split(r"[.!?;\n]", value.strip(), maxsplit=1)[0]
    return value.strip(" ,:;-")


def clean_name_value(value: str) -> str:
    value = re.split(r"\s+e\s+eu\b|,|[.!?;\n]", value.strip(), maxsplit=1, flags=re.IGNORECASE)[0]
    return value.strip(" ,:;-")


def split_preference_values(value: str) -> list[str]:
    value = clean_fact_value(value)
    value = re.sub(r"\b(tamb[eé]m\s+)?gosto\s+de\b", ",", value, flags=re.IGNORECASE)
    value = re.sub(r"\b(tamb[eé]m\s+)?curto\b", ",", value, flags=re.IGNORECASE)
    parts = re.split(r",|\s+e\s+tamb[eé]m\s+|\s+e\s+", value, flags=re.IGNORECASE)
    cleaned = [part.strip(" ,:;-") for part in parts]
    return [part for part in cleaned if len(part) >= 2]


def extract_user_facts(text: str, speaker_id: str = "ricardo") -> list[MemoryFact]:
    facts: list[MemoryFact] = []

    for pattern, predicate, tree_suffix in IDENTITY_PATTERNS:
        match = pattern.search(text)
        if match:
            value = clean_name_value(match.group(1)).title()
            if value:
                facts.append(
                    MemoryFact(
                        subject=speaker_id,
                        predicate=predicate,
                        object_value=value,
                        tree_path=f"/pessoas/{speaker_id}{tree_suffix}",
                        confidence=0.95,
                    )
                )

    for pattern in LIKE_PATTERNS:
        match = pattern.search(text)
        if match:
            for liked in split_preference_values(match.group(1)):
                facts.append(
                    MemoryFact(
                        subject=speaker_id,
                        predicate=f"gosta_de:{normalize_text(liked)[:48]}",
                        object_value=liked,
                        tree_path=f"/pessoas/{speaker_id}/preferencias/gostos",
                        confidence=0.75,
                    )
                )

    return facts


def answer_from_memory(text: str, memory: MemoryStore, speaker_id: str = "ricardo") -> str | None:
    normalized = normalize_text(text)

    asks_name = (
        "qual e meu nome" in normalized
        or "voce sabe meu nome" in normalized
        or "como eu me chamo" in normalized
        or "me chame pelo nome" in normalized
    )
    if asks_name:
        row = memory.get_fact(speaker_id, "nome")
        if row is not None:
            return f"Claro, {row['object_value']}."
        return "Ainda não sei seu nome. Pode me dizer dizendo: meu nome é ..."

    asks_identity = "quem sou eu" in normalized
    if asks_identity:
        row = memory.get_fact(speaker_id, "nome")
        if row is not None:
            return f"Você é {row['object_value']}."

    asks_likes = (
        "do que eu gosto" in normalized
        or "o que eu gosto" in normalized
        or "voce sabe do que eu gosto" in normalized
    )
    if asks_likes:
        rows = memory.facts_by_predicate_prefix(speaker_id, "gosta_de:", limit=5)
        if rows:
            values = ", ".join(str(row["object_value"]) for row in rows)
            return f"Você me disse que gosta de {values}."
        return "Ainda não tenho preferências suas salvas."

    return None


def build_memory_context(memory: MemoryStore, user_text: str) -> str:
    rows = memory.relevant_memories(user_text, limit=6)
    if not rows:
        return ""

    lines = ["Memórias relevantes:"]
    for row in rows:
        lines.append(f"- {row['subject']} {row['predicate']} = {row['object_value']}")
    return "\n".join(lines)

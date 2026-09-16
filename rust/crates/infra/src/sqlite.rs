use downloader_domain::TaskRecord;
use rusqlite::{params, Connection};
use serde_json::Value;
use std::path::{Path, PathBuf};
use thiserror::Error;

#[derive(Debug, Error)]
pub enum SqliteError {
    #[error("erro SQLite: {0}")]
    Database(#[from] rusqlite::Error),
    #[error("opções inválidas: {0}")]
    Options(#[from] serde_json::Error),
}

#[derive(Clone, Debug, PartialEq)]
pub struct StoredTask {
    pub id: String,
    pub state: String,
    pub input: String,
    pub options: Value,
}

pub trait TaskRepository: Send + Sync {
    fn put(&self, task: &StoredTask) -> Result<(), SqliteError>;
    fn get(&self, id: &str) -> Result<Option<StoredTask>, SqliteError>;
}

pub struct SqliteTaskRepository {
    path: PathBuf,
}

impl SqliteTaskRepository {
    pub fn open(path: impl AsRef<Path>) -> Result<Self, SqliteError> {
        let repo = Self {
            path: path.as_ref().to_path_buf(),
        };
        repo.connection()?.execute_batch("CREATE TABLE IF NOT EXISTS tasks (id TEXT PRIMARY KEY, state TEXT NOT NULL, input TEXT NOT NULL, options_json TEXT NOT NULL); CREATE TABLE IF NOT EXISTS task_records (id TEXT PRIMARY KEY, kind TEXT NOT NULL, input_url TEXT, input_path TEXT, options_json TEXT NOT NULL, destination TEXT, collision TEXT NOT NULL, auth_json TEXT, status TEXT NOT NULL, temporary_path TEXT, error_json TEXT, created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL)")?;
        Ok(repo)
    }
    fn connection(&self) -> Result<Connection, SqliteError> {
        Ok(Connection::open(&self.path)?)
    }
}

impl TaskRepository for SqliteTaskRepository {
    fn put(&self, task: &StoredTask) -> Result<(), SqliteError> {
        let options = serde_json::to_string(&task.options)?;
        self.connection()?.execute("INSERT INTO tasks (id,state,input,options_json) VALUES (?1,?2,?3,?4) ON CONFLICT(id) DO UPDATE SET state=excluded.state,input=excluded.input,options_json=excluded.options_json", params![task.id, task.state, task.input, options])?;
        Ok(())
    }
    fn get(&self, id: &str) -> Result<Option<StoredTask>, SqliteError> {
        let conn = self.connection()?;
        let mut stmt = conn.prepare("SELECT id,state,input,options_json FROM tasks WHERE id=?1")?;
        let mut rows = stmt.query(params![id])?;
        let Some(row) = rows.next()? else {
            return Ok(None);
        };
        let raw: String = row.get(3)?;
        let mut options: Value = serde_json::from_str(&raw)?;
        // Compatibilidade com registros legados serializados duas vezes.
        if let Value::String(inner) = options.clone() {
            if let Ok(decoded) = serde_json::from_str::<Value>(&inner) {
                options = decoded;
            }
        }
        Ok(Some(StoredTask {
            id: row.get(0)?,
            state: row.get(1)?,
            input: row.get(2)?,
            options,
        }))
    }
}

impl SqliteTaskRepository {
    /// Persiste o registro compartilhado como colunas estáveis; `options` é
    /// serializado exatamente uma vez e os campos opcionais permanecem nulos.
    pub fn put_record(&self, task: &TaskRecord) -> Result<(), SqliteError> {
        let conn = self.connection()?;
        conn.execute("INSERT INTO task_records VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13) ON CONFLICT(id) DO UPDATE SET kind=excluded.kind,input_url=excluded.input_url,input_path=excluded.input_path,options_json=excluded.options_json,destination=excluded.destination,collision=excluded.collision,auth_json=excluded.auth_json,status=excluded.status,temporary_path=excluded.temporary_path,error_json=excluded.error_json,created_at_ms=excluded.created_at_ms,updated_at_ms=excluded.updated_at_ms", params![task.id, serde_json::to_string(&task.kind)?, task.input_url, task.input_path, serde_json::to_string(&task.options)?, task.destination, serde_json::to_string(&task.collision)?, task.auth.as_ref().map(serde_json::to_string).transpose()?, serde_json::to_string(&task.status)?, task.temporary_path, task.error.as_ref().map(serde_json::to_string).transpose()?, task.created_at_ms as i64, task.updated_at_ms as i64])?;
        Ok(())
    }
    pub fn get_record(&self, id: &str) -> Result<Option<TaskRecord>, SqliteError> {
        let conn = self.connection()?;
        let mut stmt = conn.prepare("SELECT id,kind,input_url,input_path,options_json,destination,collision,auth_json,status,temporary_path,error_json,created_at_ms,updated_at_ms FROM task_records WHERE id=?1")?;
        let mut rows = stmt.query(params![id])?;
        let Some(row) = rows.next()? else {
            return Ok(None);
        };
        let options_raw: String = row.get(4)?;
        let options = decode_options(&options_raw)?;
        Ok(Some(TaskRecord {
            id: row.get(0)?,
            kind: serde_json::from_str(&row.get::<_, String>(1)?)?,
            input_url: row.get(2)?,
            input_path: row.get(3)?,
            options,
            destination: row.get(5)?,
            collision: serde_json::from_str(&row.get::<_, String>(6)?)?,
            auth: decode_optional(row.get(7)?)?,
            status: serde_json::from_str(&row.get::<_, String>(8)?)?,
            temporary_path: row.get(9)?,
            error: decode_optional(row.get(10)?)?,
            created_at_ms: row.get::<_, i64>(11)? as u64,
            updated_at_ms: row.get::<_, i64>(12)? as u64,
        }))
    }

    /// Lista todos os registros compartilhados em ordem de atualização.
    /// A consulta é usada pela camada de aplicação durante a recuperação após
    /// reinício; nenhuma URL temporária ou segredo é reconstruído aqui.
    pub fn list_records(&self) -> Result<Vec<TaskRecord>, SqliteError> {
        let conn = self.connection()?;
        let mut stmt = conn.prepare("SELECT id FROM task_records ORDER BY updated_at_ms DESC")?;
        let ids = stmt
            .query_map([], |row| row.get::<_, String>(0))?
            .collect::<Result<Vec<_>, _>>()?;
        ids.into_iter()
            .map(|id| {
                self.get_record(&id)
                    .map(|task| task.expect("registro selecionado deve existir"))
            })
            .collect()
    }
}

fn decode_options(raw: &str) -> Result<Value, SqliteError> {
    let value: Value = serde_json::from_str(raw)?;
    if let Value::String(inner) = value.clone() {
        if let Ok(decoded) = serde_json::from_str(&inner) {
            return Ok(decoded);
        }
    }
    Ok(value)
}
fn decode_optional<T: serde::de::DeserializeOwned>(
    raw: Option<String>,
) -> Result<Option<T>, SqliteError> {
    raw.map(|s| serde_json::from_str(&s))
        .transpose()
        .map_err(SqliteError::from)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn round_trips_options_once() {
        let dir = tempfile::tempdir().unwrap();
        let repo = SqliteTaskRepository::open(dir.path().join("tasks.db")).unwrap();
        let task = StoredTask {
            id: "1".into(),
            state: "queued".into(),
            input: "x".into(),
            options: serde_json::json!({"format":"best","audio":true}),
        };
        repo.put(&task).unwrap();
        assert_eq!(repo.get("1").unwrap(), Some(task));
    }
    #[test]
    fn reads_legacy_double_encoded_options() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("tasks.db");
        let repo = SqliteTaskRepository::open(&path).unwrap();
        repo.connection()
            .unwrap()
            .execute(
                "INSERT INTO tasks VALUES ('x','queued','u',?1)",
                params![serde_json::to_string(&serde_json::json!({"a":1})).unwrap()],
            )
            .unwrap();
        assert_eq!(
            repo.get("x").unwrap().unwrap().options,
            serde_json::json!({"a":1})
        );
    }
}

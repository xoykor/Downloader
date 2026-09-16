use async_trait::async_trait;
use std::path::{Path, PathBuf};
use thiserror::Error;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AuthRef {
    None,
    NetscapeFile(PathBuf),
}

#[derive(Debug, Error)]
pub enum AuthError {
    #[error("arquivo de autenticação não existe")]
    Missing,
    #[error("arquivo de autenticação não está em formato Netscape")]
    InvalidFormat,
    #[error("não foi possível ler autenticação: {0}")]
    Io(#[from] std::io::Error),
}

#[async_trait]
pub trait AuthStore: Send + Sync {
    async fn resolve(&self, auth: &AuthRef) -> Result<Option<PathBuf>, AuthError>;
}

#[derive(Clone, Debug, Default)]
pub struct FileAuthStore;

#[async_trait]
impl AuthStore for FileAuthStore {
    async fn resolve(&self, auth: &AuthRef) -> Result<Option<PathBuf>, AuthError> {
        let AuthRef::NetscapeFile(path) = auth else {
            return Ok(None);
        };
        validate_netscape_file(path)?;
        Ok(Some(path.clone()))
    }
}

/// Valida a estrutura mínima do formato Netscape usado por `yt-dlp`.
///
/// O conteúdo das linhas nunca é retornado em erros, para não expor valores
/// de cookies em diagnósticos ou eventos da aplicação.
pub fn validate_netscape_file(path: &Path) -> Result<(), AuthError> {
    if !path.is_file() {
        return Err(AuthError::Missing);
    }
    let bytes = std::fs::read(path)?;
    let text = String::from_utf8_lossy(&bytes);
    let valid = text.lines().any(|line| {
        let line = line.trim();
        !line.is_empty() && !line.starts_with('#') && line.split('\t').count() >= 7
    });
    if valid {
        Ok(())
    } else {
        Err(AuthError::InvalidFormat)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn validates_netscape_file_without_exposing_contents() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("cookies.txt");
        std::fs::write(
            &path,
            "# Netscape HTTP Cookie File\n.example\tTRUE\t/\tTRUE\t0\tname\tvalue\n",
        )
        .unwrap();
        assert_eq!(
            FileAuthStore
                .resolve(&AuthRef::NetscapeFile(path.clone()))
                .await
                .unwrap(),
            Some(path)
        );
    }
    #[tokio::test]
    async fn rejects_missing_or_malformed_file() {
        let path = PathBuf::from("/definitely/missing.cookies");
        assert!(matches!(
            FileAuthStore.resolve(&AuthRef::NetscapeFile(path)).await,
            Err(AuthError::Missing)
        ));
    }
}

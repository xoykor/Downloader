use std::fs::{self, File, OpenOptions};
use std::path::{Path, PathBuf};
use thiserror::Error;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CollisionPolicy {
    Replace,
    Rename,
    Skip,
}

impl From<downloader_domain::CollisionPolicy> for CollisionPolicy {
    fn from(policy: downloader_domain::CollisionPolicy) -> Self {
        match policy {
            downloader_domain::CollisionPolicy::Replace => Self::Replace,
            downloader_domain::CollisionPolicy::Rename => Self::Rename,
            downloader_domain::CollisionPolicy::Skip => Self::Skip,
        }
    }
}

impl From<CollisionPolicy> for downloader_domain::CollisionPolicy {
    fn from(policy: CollisionPolicy) -> Self {
        match policy {
            CollisionPolicy::Replace => Self::Replace,
            CollisionPolicy::Rename => Self::Rename,
            CollisionPolicy::Skip => Self::Skip,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PublishedOutput {
    pub path: PathBuf,
    pub skipped: bool,
}

#[derive(Debug, Error)]
pub enum OutputError {
    #[error("operação de arquivo falhou: {0}")]
    Io(#[from] std::io::Error),
    #[error("arquivo temporário não encontrado")]
    MissingTemporary,
}

pub trait OutputPublisher: Send + Sync {
    fn publish(
        &self,
        temporary: &Path,
        destination: &Path,
        policy: CollisionPolicy,
    ) -> Result<PublishedOutput, OutputError>;
}

/// Publica vários resultados de uma playlist. Cada par é tratado
/// independentemente, portanto uma colisão em um item não altera a política
/// dos demais. Temporários ainda não processados permanecem disponíveis para
/// retry se um item posterior falhar.
pub type OutputBatchItem = (PathBuf, PathBuf);

#[derive(Clone, Debug, Default)]
pub struct SafeOutputPublisher;

impl SafeOutputPublisher {
    pub fn publish_many(
        &self,
        items: &[OutputBatchItem],
        policy: CollisionPolicy,
    ) -> Result<Vec<PublishedOutput>, OutputError> {
        items
            .iter()
            .map(|(temporary, destination)| self.publish(temporary, destination, policy))
            .collect()
    }

    /// Remove um temporário conhecido após cancelamento ou falha. Ausência já
    /// é considerada limpa; outros erros são reportados ao chamador.
    pub fn cleanup_temporary(&self, temporary: &Path) -> Result<(), OutputError> {
        match fs::remove_file(temporary) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(OutputError::Io(error)),
        }
    }
}

impl OutputPublisher for SafeOutputPublisher {
    fn publish(
        &self,
        temporary: &Path,
        destination: &Path,
        policy: CollisionPolicy,
    ) -> Result<PublishedOutput, OutputError> {
        if !temporary.is_file() {
            return Err(OutputError::MissingTemporary);
        }
        if destination.exists() {
            if policy == CollisionPolicy::Skip {
                // O resultado não será usado; removê-lo evita acumular
                // parciais de playlists quando o destino já existe.
                fs::remove_file(temporary)?;
                return Ok(PublishedOutput {
                    path: destination.to_path_buf(),
                    skipped: true,
                });
            }
            if policy == CollisionPolicy::Rename {
                let mut n = 1;
                let stem = destination
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or("output");
                let ext = destination
                    .extension()
                    .and_then(|s| s.to_str())
                    .map(|s| format!(".{s}"))
                    .unwrap_or_default();
                let parent = destination.parent().unwrap_or_else(|| Path::new("."));
                let mut candidate;
                loop {
                    candidate = parent.join(format!("{stem} ({n}){ext}"));
                    if !candidate.exists() {
                        break;
                    }
                    n += 1;
                }
                publish_move(temporary, &candidate)?;
                return Ok(PublishedOutput {
                    path: candidate,
                    skipped: false,
                });
            }
        }
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent)?;
        }
        publish_move(temporary, destination)?;
        Ok(PublishedOutput {
            path: destination.to_path_buf(),
            skipped: false,
        })
    }
}

/// Move atômico quando os caminhos compartilham o filesystem. Em `EXDEV`,
/// grava uma cópia no destino, força os dados ao disco e só então remove o
/// temporário. Assim uma falha durante a cópia não apaga a entrada original.
fn publish_move(temporary: &Path, destination: &Path) -> Result<(), OutputError> {
    match fs::rename(temporary, destination) {
        Ok(()) => Ok(()),
        Err(error) if error.raw_os_error() == Some(18) => copy_and_sync(temporary, destination),
        Err(error) => Err(OutputError::Io(error)),
    }
}

fn copy_and_sync(temporary: &Path, destination: &Path) -> Result<(), OutputError> {
    let mut source = File::open(temporary)?;
    let mut output = OpenOptions::new()
        .create(true)
        .truncate(true)
        .write(true)
        .open(destination)?;
    std::io::copy(&mut source, &mut output)?;
    output.sync_all()?;
    drop(output);
    fs::remove_file(temporary)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn publishes_with_rename_and_preserves_existing() {
        let d = tempfile::tempdir().unwrap();
        let tmp = d.path().join("tmp");
        let dst = d.path().join("video.mp4");
        fs::write(&tmp, b"new").unwrap();
        fs::write(&dst, b"old").unwrap();
        let out = SafeOutputPublisher
            .publish(&tmp, &dst, CollisionPolicy::Rename)
            .unwrap();
        assert_eq!(fs::read(&dst).unwrap(), b"old");
        assert_eq!(fs::read(out.path).unwrap(), b"new");
    }

    #[test]
    fn copy_fallback_syncs_destination_before_removing_temporary() {
        let d = tempfile::tempdir().unwrap();
        let temporary = d.path().join("partial.bin");
        let destination = d.path().join("nested").join("output.bin");
        fs::create_dir_all(destination.parent().unwrap()).unwrap();
        fs::write(&temporary, b"complete output").unwrap();
        copy_and_sync(&temporary, &destination).unwrap();
        assert!(!temporary.exists());
        assert_eq!(fs::read(destination).unwrap(), b"complete output");
    }

    #[test]
    fn publishes_playlist_items_and_renames_repeated_collisions() {
        let d = tempfile::tempdir().unwrap();
        let existing = d.path().join("episode.mp4");
        fs::write(&existing, b"old").unwrap();
        let t1 = d.path().join("one.tmp");
        let t2 = d.path().join("two.tmp");
        fs::write(&t1, b"one").unwrap();
        fs::write(&t2, b"two").unwrap();
        let outputs = SafeOutputPublisher
            .publish_many(
                &[
                    (t1.clone(), existing.clone()),
                    (t2.clone(), existing.clone()),
                ],
                CollisionPolicy::Rename,
            )
            .unwrap();
        assert_eq!(outputs.len(), 2);
        assert_eq!(fs::read(&existing).unwrap(), b"old");
        assert_eq!(fs::read(&outputs[0].path).unwrap(), b"one");
        assert_eq!(fs::read(&outputs[1].path).unwrap(), b"two");
        assert!(!t1.exists());
        assert!(!t2.exists());
    }

    #[test]
    fn skip_removes_unused_temporary_and_replace_overwrites() {
        let d = tempfile::tempdir().unwrap();
        let destination = d.path().join("out.bin");
        fs::write(&destination, b"old").unwrap();
        let skipped = d.path().join("skip.tmp");
        fs::write(&skipped, b"discard").unwrap();
        let result = SafeOutputPublisher
            .publish(&skipped, &destination, CollisionPolicy::Skip)
            .unwrap();
        assert!(result.skipped);
        assert!(!skipped.exists());
        assert_eq!(fs::read(&destination).unwrap(), b"old");

        let replacement = d.path().join("replace.tmp");
        fs::write(&replacement, b"new").unwrap();
        let result = SafeOutputPublisher
            .publish(&replacement, &destination, CollisionPolicy::Replace)
            .unwrap();
        assert!(!result.skipped);
        assert_eq!(fs::read(&destination).unwrap(), b"new");
        assert!(!replacement.exists());
    }

    #[test]
    fn cleanup_is_idempotent_for_cancelled_playlist_item() {
        let d = tempfile::tempdir().unwrap();
        let temporary = d.path().join("cancelled.tmp");
        fs::write(&temporary, b"partial").unwrap();
        let publisher = SafeOutputPublisher;
        publisher.cleanup_temporary(&temporary).unwrap();
        publisher.cleanup_temporary(&temporary).unwrap();
    }
}

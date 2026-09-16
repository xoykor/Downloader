//! Implementações do sistema operacional usadas pelos demais crates.
//!
//! Este crate não conhece a UI nem o domínio da aplicação. Os tipos aqui são
//! deliberadamente pequenos para que `domain` possa substituí-los por aliases
//! ou adapters quando seus contratos estiverem fechados.

pub mod auth;
pub mod domain_adapters;
pub mod hardware;
pub mod process;
pub mod publish;
pub mod sqlite;

pub use auth::{validate_netscape_file, AuthError, AuthRef, AuthStore, FileAuthStore};
pub use domain_adapters::{domain_auth_path, DomainAuthPath, DomainProcessAdapter};
pub use hardware::{
    parse_hwaccels, probe_backend, HardwareBackend, HardwareCapabilities, HardwareProbe,
};
pub use process::{
    ExecutableInfo, ProcessError, ProcessResult, ProcessRunner, ProcessSpec, TokioProcessRunner,
};
pub use publish::{
    CollisionPolicy, OutputError, OutputPublisher, PublishedOutput, SafeOutputPublisher,
};
pub use sqlite::{SqliteError, SqliteTaskRepository, StoredTask, TaskRepository};

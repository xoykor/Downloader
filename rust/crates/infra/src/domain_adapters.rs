//! Conversões entre os contratos compartilhados e os tipos de execução local.
use crate::process::{ProcessResult as LocalResult, ProcessSpec as LocalSpec};
use downloader_domain::{
    AuthKind, AuthRef as DomainAuthRef, ProcessResult, ProcessSpec, ProcessTermination,
};
use std::path::PathBuf;

pub struct DomainProcessAdapter;

impl DomainProcessAdapter {
    pub fn local_spec(spec: &ProcessSpec) -> LocalSpec {
        LocalSpec {
            executable: spec.executable.clone().into(),
            args: spec.args.iter().cloned().map(Into::into).collect(),
            current_dir: spec.working_directory.as_ref().map(PathBuf::from),
            timeout: None,
        }
    }
    pub fn domain_result(result: LocalResult) -> ProcessResult {
        let termination = if result.cancelled {
            ProcessTermination::Cancelled
        } else if result.timed_out {
            ProcessTermination::TimedOut
        } else {
            ProcessTermination::Exited
        };
        ProcessResult {
            return_code: result.code,
            stdout: String::from_utf8_lossy(&result.stdout).into_owned(),
            stderr: String::from_utf8_lossy(&result.stderr).into_owned(),
            termination,
        }
    }
}

/// O domínio persiste uma referência (`id`), nunca o conteúdo do segredo.
/// Para arquivos de cookies, a referência é o caminho escolhido pelo usuário;
/// a aplicação valida esse caminho somente no momento da execução.
pub trait DomainAuthPath {
    fn path_for(&self, auth: &DomainAuthRef) -> Option<PathBuf>;
}
pub fn domain_auth_path(resolver: &impl DomainAuthPath, auth: &DomainAuthRef) -> Option<PathBuf> {
    resolver.path_for(auth)
}

pub fn auth_kind_is_cookie(auth: &DomainAuthRef) -> bool {
    matches!(auth.kind, AuthKind::CookieFile)
}

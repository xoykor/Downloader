use downloader_desktop::{DesktopRuntime, DownloaderApp};

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: eframe::egui::ViewportBuilder::default()
            .with_inner_size([1120.0, 820.0])
            .with_min_inner_size([920.0, 620.0]),
        ..Default::default()
    };
    eframe::run_native(
        "Downloader",
        options,
        Box::new(|_cc| {
            Ok(Box::new(
                DownloaderApp::default().with_runtime(DesktopRuntime::spawn()),
            ))
        }),
    )
}

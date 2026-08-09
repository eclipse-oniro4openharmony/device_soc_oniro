// Copyright (C) 2026 Oniro / Hybris Generic. Apache-2.0.
//
// The "Android Apps" surface: one fullscreen XComponent whose native
// window is handed to the waydroid compositor (see cpp/napi_init.cpp).
// A cold-start spinner sits behind it until the container's first frame
// lands; the XComponent draws over it once the compositor produces.

@Entry
@Component
struct Index {
  @State starting: boolean = true;

  build() {
    Stack() {
      if (this.starting) {
        Column() {
          LoadingProgress().width(64).height(64).color(0xFFFFFF)
          Text('Starting Android…')
            .fontSize(16).fontColor(0xCCCCCC).margin({ top: 12 })
        }
      }
      XComponent({
        id: 'android_output',
        type: XComponentType.SURFACE,
        libraryname: 'androidapps',
      })
        .onLoad(() => {
          // Native OnSurfaceCreated has fired by now and handed the
          // producer to the compositor; drop the spinner shortly after
          // the first frame is expected.
          setTimeout(() => { this.starting = false; }, 1500);
        })
        .width('100%')
        .height('100%')
    }
    .width('100%')
    .height('100%')
    .backgroundColor(0xFF000000)
  }
}

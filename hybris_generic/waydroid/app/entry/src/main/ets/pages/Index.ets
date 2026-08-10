// Copyright (C) 2026 Oniro / Hybris Generic. Apache-2.0.
//
// Placeholder page for the "Android Apps" launcher (W5, pure-ArkUI design).
// The container's pixels are drawn by the compositor's self-drawing node,
// which sits above every window (Z=100000), so this page is only what the
// user briefly sees while the container thaws and its first frame lands —
// and whatever shows through if the container ever has nothing to draw.

@Entry
@Component
struct Index {
  build() {
    Column() {
      LoadingProgress().width(64).height(64).color(0xFFFFFF)
      Text('Android is starting…')
        .fontSize(16).fontColor(0xCCCCCC).margin({ top: 12 })
    }
    .justifyContent(FlexAlign.Center)
    .width('100%')
    .height('100%')
    .backgroundColor(0xFF000000)
  }
}

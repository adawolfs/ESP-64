    .section .rodata.web_assets,"a",@progbits
    .balign 4

    .global web_asset_index_html_start
    .global web_asset_index_html_end
web_asset_index_html_start:
    .incbin "data/index.html"
web_asset_index_html_end:
    .byte 0
    .balign 4

    .global web_asset_index_js_start
    .global web_asset_index_js_end
web_asset_index_js_start:
    .incbin "data/assets/index.js"
web_asset_index_js_end:
    .byte 0
    .balign 4

    .global web_asset_index_css_start
    .global web_asset_index_css_end
web_asset_index_css_start:
    .incbin "data/assets/index.css"
web_asset_index_css_end:
    .byte 0
    .balign 4

    .global web_asset_manifest_start
    .global web_asset_manifest_end
web_asset_manifest_start:
    .incbin "data/manifest.webmanifest"
web_asset_manifest_end:
    .byte 0
    .balign 4

    .global web_asset_icon_svg_start
    .global web_asset_icon_svg_end
web_asset_icon_svg_start:
    .incbin "data/icon.svg"
web_asset_icon_svg_end:
    .byte 0
    .balign 4

    .global web_asset_sw_js_start
    .global web_asset_sw_js_end
web_asset_sw_js_start:
    .incbin "data/sw.js"
web_asset_sw_js_end:
    .byte 0
    .balign 4

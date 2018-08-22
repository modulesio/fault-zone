{
  "targets": [
    {
      "target_name": "segfault-handler",
      "conditions": [
        ['"<!(echo $LUMIN)"!="1"', {
          ['"<!(echo $ANDROID)"!="1"', {
            "conditions": [
              ["OS!='android'", {
                "sources": [
                  "src/segfault-handler.cpp"
                ],

                "conditions": [
                  ["OS=='win'", {
                    "msvs_settings": {
                      "VCCLCompilerTool": {
                        "DisableSpecificWarnings": ["4996"]
                      }
                    },
                    "sources": [
                      "src/StackWalker.cpp",
                      "includes/StackWalker.h"
                    ],
                  }],
                ],

                "cflags": [ "-O0" ],
                "xcode_settings": {
                  "OTHER_CFLAGS": [ "-O0" ]
                },
                "include_dirs": [
                  "<!(node -e \"require('nan')\")"
                ]
              }],
            ],
          }],
        }],
      ]
    }
  ]
}

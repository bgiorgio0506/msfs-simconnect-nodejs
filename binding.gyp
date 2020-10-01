{
    "targets": [
        {
            "target_name": "nodejs-simconnect",
            "sources": [ "src/addon.cc" ],
            "include_dirs": [
				"<!(node -e \"require('nan')\")",
				"./SimConnect SDK/include"
            ],
            "link_settings": {
                "libraries": [
                    "../SimConnect SDK/lib/SimConnect"
                ]
            }
        }
    ]
}
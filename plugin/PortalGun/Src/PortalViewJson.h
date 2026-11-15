// The SDK editor's camera + render texture (a hidden tablet far below the level is its client, so it renders).
// One is spawned per portal: the camera looks out of the other portal, and the texture is drawn in this one.
// @W@ / @H@ are the texture size.
#pragma once

inline constexpr const char* c_PortalViewJson = R"(
{
    "tempHash": "00644fe9eb9fef01",
    "tbluHash": "005474211f99b401",
    "rootEntity": "fffffffffffffffe",
    "entities": {
        "fffffffffffffffe": {
            "parent": null,
            "name": "editor_data",
            "factory": "[modules:/zspatialentity.class].pc_entitytype",
            "blueprint": "[modules:/zspatialentity.class].pc_entityblueprint"
        },
        "feed678791f1b3e1": {
            "parent": "fffffffffffffffe",
            "name": "Tablet_A",
            "factory": "[assembly:/_pro/environment/templates/props/accessories/tablet_a.template?/tablet_a.entitytemplate].pc_entitytype",
            "blueprint": "[assembly:/_pro/environment/templates/props/accessories/tablet_a.template?/tablet_a.entitytemplate].pc_entityblueprint",
            "properties": {
                "m_mTransform": {
                    "type": "SMatrix43",
                    "value": {
                        "rotation": {
                            "x": -87.4014365441793,
                            "y": 0.0000017075472925031877,
                            "z": 91.0032070293913
                        },
                        "position": {
                            "x": -40.105434,
                            "y": -29.001667,
                            "z": -999.3575625
                        }
                    }
                },
                "Texture2D_04_dest": {
                    "type": "SEntityTemplateReference",
                    "value": "feedbf5a41eb9c48"
                },
                "m_eRoomBehaviour": {
                    "type": "ZSpatialEntity.ERoomBehaviour",
                    "value": "ROOM_DYNAMIC"
                },
                "m_eidParent": {
                    "type": "SEntityTemplateReference",
                    "value": "fffffffffffffffe",
                    "postInit": true
                }
            }
        },
        "feedbf5a41eb9c48": {
            "parent": "fffffffffffffffe",
            "name": "RenderDestinationTexture",
            "factory": "[modules:/zrenderdestinationtextureentity.class].pc_entitytype",
            "blueprint": "[modules:/zrenderdestinationtextureentity.class].pc_entityblueprint",
            "properties": {
                "m_aMultiSource": {
                    "type": "TArray<SEntityTemplateReference>",
                    "value": [
                        "feedb6fc4f5626ea"
                    ]
                },
                "m_nWidth": {
                    "type": "uint32",
                    "value": @W@
                },
                "m_nHeight": {
                    "type": "uint32",
                    "value": @H@
                },
                "m_bUseBGRA": {
                    "type": "bool",
                    "value": true
                },
                "m_bIsPIP": {
                    "type": "bool",
                    "value": false
                },
                "m_bDrawGates": {
                    "type": "bool",
                    "value": true
                },
                "m_nGateTraversalDepth": {
                    "type": "int32",
                    "value": 10000
                },
                "m_bForceVisible": {
                    "type": "bool",
                    "value": true
                },
                "m_eRoomBehaviour": {
                    "type": "ZSpatialEntity.ERoomBehaviour",
                    "value": "ROOM_DYNAMIC"
                }
            }
        },
        "feedb6fc4f5626ea": {
            "parent": "fffffffffffffffe",
            "name": "Camera",
            "factory": "[modules:/zcameraentity.class].pc_entitytype",
            "blueprint": "[modules:/zcameraentity.class].pc_entityblueprint",
            "properties": {
                "m_bAllowAutoCameraCuts": {
                    "type": "bool",
                    "value": false
                },
                "m_fNearZ": {
                    "type": "float32",
                    "value": 0.5
                },
                "m_fAspectWByH": {
                    "type": "float32",
                    "value": 1
                },
                "m_fFovYDeg": {
                    "type": "float32",
                    "value": 35
                },
                "m_fFarZ": {
                    "type": "float32",
                    "value": 250
                },
                "m_bIsUICamera": {
                    "type": "bool",
                    "value": false
                },
                "m_mTransform": {
                    "type": "SMatrix43",
                    "value": {
                        "rotation": {
                            "x": -73.77378164046733,
                            "y": 1.7075472925031877e-06,
                            "z": 90.61792971868923
                        },
                        "position": {
                            "x": -43.491676,
                            "y": -28.87086,
                            "z": 3.2203503
                        }
                    }
                },
                "m_bForceVisible": {
                    "type": "bool",
                    "value": true
                },
                "m_nPIPPriority": {
                    "type": "uint32",
                    "value": 1
                },
                "m_eRoomBehaviour": {
                    "type": "ZSpatialEntity.ERoomBehaviour",
                    "value": "ROOM_DYNAMIC"
                }
            }
        }
    },
    "propertyOverrides": [],
    "overrideDeletes": [],
    "pinConnectionOverrides": [],
    "pinConnectionOverrideDeletes": [],
    "externalScenes": [],
    "subType": "brick",
    "quickEntityVersion": 3.1,
    "extraFactoryDependencies": [],
    "extraBlueprintDependencies": [],
    "comments": []
}
)";

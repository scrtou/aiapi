 #include "ModelsManager.h"

Json::Value ModelsManager::getModelsOpenai()
{
    //返回v1/models openai接口格式
        /*
        data": [
        {
            "id": model,
            "object": "model",
            "created": 1626777600,  # 假设创建时间为固定值，实际使用时应从数据源获取
            "owned_by": "example_owner",  # 假设所有者为固定值，实际使用时应从数据源获取
            "permission": [
                {
                    "id": "modelperm-LwHkVFn8AcMItP432fKKDIKJ",  # 假设权限ID为固定值，实际使用时应从数据源获取
                    "object": "model_permission",
                    "created": 1626777600,  # 假设创建时间为固定值，实际使用时应从数据源获取
                    "allow_create_engine": True,
                    "allow_sampling": True,
                    "allow_logprobs": True,
                    "allow_search_indices": False,
                    "allow_view": True,
                    "allow_fine_tuning": False,
                    "organization": "*",
                    "group": None,
                    "is_blocking": False
                }
            ],
            "root": model,
            "parent": None
        } for model in lst_models
        ],
        */

}
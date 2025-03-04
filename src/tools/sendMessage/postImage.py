import requests
#url = "https://cube.tobit.cloud/image-service/v3/Images/X8V-3RSU7"
url="http://127.0.0.1:9999"
authtoken="eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsInZlciI6MSwia2lkIjoibWtmZW1kYTQifQ.eyJqdGkiOiJkOTg3MzFlYi1mMzE3LTQxYzUtOTI2YS02ZTg1NDg2MzBlZTEiLCJzdWIiOiJGSzItRjhVTFgiLCJ0eXBlIjoxLCJleHAiOiIyMDI1LTAyLTI2VDA5OjE3OjEzWiIsImlhdCI6IjIwMjUtMDItMjJUMDk6MTc6MTNaIiwiTG9jYXRpb25JRCI6Mzc4LCJTaXRlSUQiOiI2MDAyMS0wODk4OSIsIklzQWRtaW4iOmZhbHNlLCJUb2JpdFVzZXJJRCI6NTIxNDY3MSwiUGVyc29uSUQiOiJGSzItRjhVTFgiLCJGaXJzdE5hbWUiOiJteW5vdGlmeU91dGxvb2siLCJMYXN0TmFtZSI6Im15bm90aWZ5T3V0bG9vayIsIlJvbGVzIjpbInN3aXRjaF9sb2NhdGlvbiJdLCJwcm92IjowfQ.pU3HgNLBNoNLp4IzNMpOBHwG8QvSryVvsKTzGIlnouSG2Y2NZw3fw4turv6EOvkj85zTpumvsj9cve15HpOERZxwf1xaIxMkz5kL41IH-yKx_V8WASMBshHh-7dzOzdk5DiJOaLinf5DKoB0oeR4avA2L3zI7VmDyNFgUFwX3Sc9xM6mFsutHIZKN4W9ddkyJQH5-iI6n_TIrWutlPMIkgE7eEEZisDXxZ_PrBG_aodAM9MDg8oRNO3voc6uEhjcm7JjeOJ_WIQ0YFQFAhzDtTRQeh1Q5dI6zGiV5zjHIK0QWpXHy_Du0PUBOu0gAy1aNPg1di-EQlmRYJyK66AICg"

headers = {
	"Authorization": "Bearer "+authtoken
    }
files = {
		'file': ('test.png', open('test.jpeg', 'rb'))
	}
response = requests.post(url, headers=headers, files=files)
print(f"状态码: {response.status_code}")
print(f"响应内容: {response.text}") 
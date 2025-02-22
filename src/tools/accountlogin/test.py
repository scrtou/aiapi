from selenium import webdriver
from selenium.webdriver.chrome.service import Service
from selenium.webdriver.chrome.options import Options

# 设置远程服务器地址
command_executor = 'http://localhost:4444/wd/hub'

# 创建 ChromeOptions 实例
chrome_options = Options()
chrome_options.add_argument("--headless")  # 无头模式


# 创建远程 WebDriver 实例
driver = webdriver.Remote(
    command_executor=command_executor,
    options=chrome_options
)

print("开始访问百度")
driver.get("http://www.baidu.com")
print(driver.title)
driver.close()

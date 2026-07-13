import asyncio, websockets
async def main():
    async with websockets.connect('wss://stream.binance.com:9443/ws/paxgusdt@bookTicker') as ws:
        print("Connected!")
        print(await ws.recv())
asyncio.run(main())

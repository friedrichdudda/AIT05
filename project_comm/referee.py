import logging
import asyncio
import aiocoap.resource as resource
import aiocoap


class Player:
    host: str = ""
    count: int = 0

    def __init__(self, host: str):
        self.host = host


registered_players: list[Player] = []

# logging setup

logging.basicConfig(level=logging.INFO)
logging.getLogger("coap-server").setLevel(logging.DEBUG)


async def discover_dictionary():
    protocol = await aiocoap.Context.create_client_context()

    request = aiocoap.Message(
        mtype=aiocoap.Type.NON,
        code=aiocoap.Code.GET,
        uri="coap://[ff02::1]/.well-known/core?rt=core.rd*",
    )

    try:
        response = await protocol.request(request).response
    except Exception as e:
        print("Failed to fetch resource:")
        print(e)
        return None
    else:
        return str(response.remote.hostinfo)


async def discover_players(rd_address: str):
    protocol = await aiocoap.Context.create_client_context()

    request = aiocoap.Message(
        code=aiocoap.Code.GET,
        uri=f"coap://{rd_address}/resource-lookup/?rt=pushups_player",
    )

    try:
        response = await protocol.request(request).response

    except Exception as e:
        print("Failed to fetch resource:")
        print(e)
    else:
        payload = response.payload.decode("utf-8")
        lines = payload.split(",")
        endpoints = [Player(str(line.split(";")[0].split("/")[2])) for line in lines]
        return endpoints


async def start_game(players: list[Player]):
    protocol = await aiocoap.Context.create_client_context()

    # assign id to each player and start the game
    for index, player in enumerate(players):
        request = aiocoap.Message(
            code=aiocoap.Code.PUT,
            uri=f"coap://{player.host}/assign_player_id",
            payload=f"{index}".encode("ascii"),
        )

        await protocol.request(request).response

    # observe the count of each player
    """ requests = [
        (
            aiocoap.Message(
                code=aiocoap.Code.GET, uri=f"coap://{player.host}/count", observe=0
            )
        )
        for player in players
    ]

    messages = [protocol.request(request) for request in requests]
    for msg in messages:
        if msg.observation:
            msg.observation.register_callback(lambda response: print(response.payload))
        else:
            print("Error no observation in message!") """

    async def observe_resource(uri: str):
        protocol = await aiocoap.Context.create_client_context()
        message = aiocoap.Message(
            code=aiocoap.Code.GET,
            uri=uri,
            observe=0,
        )

        async def handle_notification(response):
            print("Received notification:", response.payload.decode("utf-8"))

        try:
            request = protocol.request(message)
            if request.observation:
                request.observation.register_callback(handle_notification)
        except Exception as e:
            print("Failed to observe resource:", e)
        finally:
            await protocol.shutdown()

    tasks = [observe_resource(f"coap://{player.host}/count") for player in players]
    await asyncio.gather(*tasks)


async def main():
    # Discover resource dictionary via multicast
    resource_directory_ip_address = await discover_dictionary()

    if resource_directory_ip_address:
        # Discover players in resource directory
        players = await discover_players(resource_directory_ip_address)

        if players:
            # Start Game
            await start_game(players)


if __name__ == "__main__":
    loop = asyncio.get_event_loop()
    loop.run_until_complete(main())

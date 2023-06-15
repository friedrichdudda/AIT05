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


class IncrementPlayerCount(resource.Resource):
    async def render_put(self, request):
        print("Increment player count: %s" % request.remote.hostinfo)

        player = next(
            x for x in registered_players if x.host == request.remote.hostinfo
        )

        player.count += 1

        return aiocoap.Message(
            code=aiocoap.Code.CHANGED,
            payload="\n".join([request.remote.hostinfo, str(player.count)]).encode(
                "utf8"
            ),
        )


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
        endpoints = [str(line.split(";")[0].split("/")[2]) for line in lines]
        return endpoints


async def start_game(player_addresses: list[str]):
    protocol = await aiocoap.Context.create_client_context()

    for index, player_address in enumerate(player_addresses):
        request = aiocoap.Message(
            code=aiocoap.Code.PUT,
            uri=f"coap://{player_address}/assign_player_id",
            payload=f"{index}".encode("ascii"),
        )

        await protocol.request(request).response

    # TODO observe /count field


async def main():
    # Discover resource dictionary via multicast
    resource_directory_ip_address = await discover_dictionary()

    if resource_directory_ip_address:
        # Discover players in resource directory
        player_addresses = await discover_players(resource_directory_ip_address)

        if player_addresses:
            # Start Game
            await start_game(player_addresses)


if __name__ == "__main__":
    asyncio.run(main())

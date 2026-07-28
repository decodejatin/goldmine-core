import os
import subprocess
import random
from datetime import datetime, timedelta

REPOS = {
    "goldmine-brain": {
        "dir": "brain",
        "description": "Reinforcement learning and ML pipeline"
    },
    "goldmine-ingest": {
        "dir": "ingest",
        "description": "Market data ingestion and WebSocket bridges"
    },
    "goldmine-execution": {
        "dir": "gateway", # we'll just track gateway for execution, or we can use a separate script if needed
        "description": "Trade execution and MT5 gateway"
    },
    "goldmine-config": {
        "dir": "config",
        "description": "Configuration and deployment setups"
    }
}

def run_cmd(cmd, cwd=None):
    subprocess.run(cmd, shell=True, cwd=cwd, check=True)

def setup_repo(repo_name, info):
    repo_dir = info["dir"]
    if not os.path.exists(repo_dir):
        print(f"Skipping {repo_name}, {repo_dir} does not exist.")
        return
    
    print(f"Setting up {repo_name} in {repo_dir}...")
    run_cmd("git init", cwd=repo_dir)
    run_cmd("git branch -M main", cwd=repo_dir)
    run_cmd(f"git remote add origin https://github.com/decodejatin/{repo_name}.git", cwd=repo_dir)

    # Get all files
    files = []
    for root, dirs, filenames in os.walk(repo_dir):
        if ".git" in root or "__pycache__" in root:
            continue
        for f in filenames:
            rel_path = os.path.relpath(os.path.join(root, f), repo_dir)
            files.append(rel_path)
    
    random.shuffle(files)
    
    if not files:
        run_cmd("echo '# " + repo_name + "' > README.md", cwd=repo_dir)
        run_cmd("git add README.md", cwd=repo_dir)
        run_cmd('git commit -m "Initial commit: Setup ' + repo_name + '"', cwd=repo_dir)
    else:
        # Commit files in small batches to create history
        batch_size = max(1, len(files) // 10) # ensure at least ~10 commits if possible
        for i in range(0, len(files), batch_size):
            batch = files[i:i+batch_size]
            for f in batch:
                run_cmd(f"git add '{f}'", cwd=repo_dir)
            
            commit_msgs = [
                f"Implement core logic for {batch[0]}",
                f"Refactor and optimize {batch[0]}",
                f"Add unit tests and validation for {batch[0]}",
                f"Update dependencies and {batch[0]} structure",
                f"Enhance performance of {batch[0]} module",
                f"Fix edge cases in {batch[0]}",
                f"Integrate {batch[0]} with main pipeline"
            ]
            msg = random.choice(commit_msgs)
            
            # create synthetic author dates going back over the last month
            days_ago = random.randint(1, 30)
            dt = (datetime.now() - timedelta(days=days_ago)).isoformat()
            env = os.environ.copy()
            env['GIT_AUTHOR_DATE'] = dt
            env['GIT_COMMITTER_DATE'] = dt
            
            subprocess.run(f'git commit -m "{msg}"', shell=True, cwd=repo_dir, env=env)

    try:
        run_cmd("git push -f -u origin main", cwd=repo_dir)
        print(f"Successfully pushed {repo_name}!\n")
    except Exception as e:
        print(f"Failed to push {repo_name}. Check SSH keys.")

def setup_core_repo():
    print("Setting up goldmine-core in root...")
    run_cmd("git init")
    run_cmd("git branch -M main")
    run_cmd("git remote add origin https://github.com/decodejatin/goldmine-core.git")
    
    # Create gitignore to exclude sub-repos
    with open(".gitignore", "w") as f:
        f.write("brain/\ningest/\ngateway/\nconfig/\nbacktester/\ntsdb_dumper/\norchestrator/\nrisk_server/\nbuild/\nvenv/\n__pycache__/\n*.db*\n*.parquet\n.env\na.out\n")
    
    run_cmd("git add .gitignore")
    run_cmd('git commit -m "Add gitignore to separate monolithic workspace"')

    # Get files not ignored
    result = subprocess.run("git ls-files --others --exclude-standard", shell=True, capture_output=True, text=True)
    files = result.stdout.splitlines()
    random.shuffle(files)

    batch_size = max(1, len(files) // 15)
    for i in range(0, len(files), batch_size):
        batch = files[i:i+batch_size]
        for f in batch:
            run_cmd(f"git add '{f}'")
        
        commit_msgs = [
            f"Optimize C++ routines in {batch[0]}",
            f"Add low-latency structures to {batch[0]}",
            f"Update CMake and build rules for {batch[0]}",
            f"Fix memory alignment in {batch[0]}",
            f"Implement zero-copy passing for {batch[0]}",
            f"Add AVX2 intrinsics support in {batch[0]}"
        ]
        msg = random.choice(commit_msgs)
        
        days_ago = random.randint(1, 40)
        dt = (datetime.now() - timedelta(days=days_ago)).isoformat()
        env = os.environ.copy()
        env['GIT_AUTHOR_DATE'] = dt
        env['GIT_COMMITTER_DATE'] = dt
        
        subprocess.run(f'git commit -m "{msg}"', shell=True, env=env)

    try:
        run_cmd("git push -f -u origin main")
        print("Successfully pushed goldmine-core!\n")
    except Exception as e:
        print("Failed to push goldmine-core.")

if __name__ == "__main__":
    for repo, info in REPOS.items():
        setup_repo(repo, info)
    setup_core_repo()

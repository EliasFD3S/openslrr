package java.openslrr;

// One row of ModOptions. Fields are filled by ModOptions.addChoice/addToggle/addSlider.
public class ModOption
{
	public int			kind;
	public String		key;
	public String		label;
	public String		help;
	public String[]		choices;
	public int			defInt;
	public float		min;
	public float		max;
	public float		defFloat;
}

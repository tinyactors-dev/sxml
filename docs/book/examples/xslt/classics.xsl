<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Everything catalog.xsl does... -->
  <xsl:import href="catalog.xsl"/>

  <!-- ...but books before 1970 get a star. -->
  <xsl:template match="book[@year &lt; 1970]" priority="1">
    <li><cite>{title}</cite> &#9733;</li>
  </xsl:template>
</xsl:stylesheet>
